// libklepton_ovrp — the OVRPlugin replacement. See kl_ovrp.h for why it is a
// replacement rather than a translation.
//
// This is the measurement stage. Unity's C# side reaches OVRPlugin through
// [DllImport("OVRPlugin")], which IL2CPP resolves with dlsym at runtime, so
// serving the soname and naming every lookup gives the real surface without
// guessing at any of the 466 exported entry points.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include "klepton.h"
#include "kl_ovrp.h"
// kl_glfb_note_eye_texture (the capture's eye-FBO seam) and
// kl_glfb_last_render_stage (which stage the guest actually drew into — see
// klovrp_EndFrame). Up here rather than beside SetupEyeTexture2 now that the
// timewarp bookkeeping needs it too.
#include "kl_glfb.h"
// kl_egl_sym (the GL gateway the eye textures are allocated through) and
// kl_egl_swap_count (whether eglSwapBuffers is this guest's presentation
// signal at all — see klovrp_end_frame_impl).
#include "kl_egl.h"
#include "kl_vulkan.h"

#define KL_OVRP_MAX 512
static struct { const char *name; unsigned calls; } g_ovrp[KL_OVRP_MAX];
static unsigned g_novrp;
// Resolution and per-frame calls happen on different guest threads (main vs
// render); without this, two concurrent inserts can split a slot and leave a
// NULL name for a later strcmp — observed as SIGSEGV inside ovrp_hit.
static pthread_mutex_t g_ovrp_mu = PTHREAD_MUTEX_INITIALIZER;

// name -> the pointer we handed the guest, so a guest function table can be
// read back by name (see klovrp_dump_vrdevice).
static struct { const char *name; void *ptr; } g_sym[KL_OVRP_MAX];
static unsigned g_nsym;
static void *kl_ovrp_sym_inner(const char *name);

static int ovrp_slot(const char *name) {
    pthread_mutex_lock(&g_ovrp_mu);
    int rc = -1;
    for (unsigned i = 0; i < g_novrp; i++)
        if (strcmp(g_ovrp[i].name, name) == 0) { rc = (int)i; goto out; }
    if (g_novrp >= KL_OVRP_MAX) goto out;
    g_ovrp[g_novrp].name = strdup(name);
    g_ovrp[g_novrp].calls = 0;
    rc = (int)g_novrp++;
out:
    pthread_mutex_unlock(&g_ovrp_mu);
    return rc;
}

// Reached through a per-name trampoline, so x0 is the entry point's own name.
// The stub tail-calls here, so this return value is the ovrp_ call's return
// value. ovrpSuccess is 0, which makes a permissive zero mean "it worked" —
// wrong in the usual way, but it is what collects the whole surface in one run.
static uint64_t klovrp_called(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    if (kl_permissive()) {
        if (s >= 0 && g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] call (permissive, returning 0): %s\n", name);
        return 0;
    }
    // Named WITH ITS CALLER. Every other entry point in this file is reached
    // from libunity or libOculusXRPlugin, and the one time that was not true it
    // cost a session: 1.40's libhaptics_sdk resolves two of these itself, on its
    // own thread, and the report said only which name it was — so "the viewer
    // crashes when a menu button highlights" and "the haptics SDK wants the PCM
    // API" had nothing joining them.
    size_t off = 0;
    const char *img = kl_addr_image(__builtin_return_address(0), &off);
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented OVRPlugin entry "
                    "point '%s', from %s+0x%zx\n",
            name, img ? img : "?", off);
    kl_ovrp_report(stderr);
    kl_fatal_prepare();
    abort();
}

// ---------------------------------------------------------------------------
// The two return conventions, and why a blanket zero is not safe here
//
// OVRPlugin entry points return one of two things, and 0 means the opposite in
// each. `ovrpResult` is 0 for success and negative for failure, so 0 is "it
// worked". `ovrpBool` is a plain boolean, so 0 is "no" — and for a getter like
// ovrp_GetInitialized that reads as "XR never came up", which is the answer we
// are specifically trying not to give.
//
// So there is no single permissive value. Entry points are placed in one of two
// named lists by which type they return, and anything in neither still aborts by
// name. Getting this wrong is the trap-6d failure again: a silent zero that reads
// as a legitimate negative answer several layers from where it was invented.
#define OVRP_SUCCESS 0
#define OVRP_TRUE    1

// The failure half of ovrpResult, by name. Every value here is one the real
// libOVRPlugin.so in this APK actually returns — the codes were counted in its
// disassembly (-1000 x90, -1001 x279, -1002 x296, -1003 x126, -1004 x70, and on
// down), which is what makes them a transcription rather than a guess. Naming
// them matters because the family below is a chain of ovrpResult returns and
// trap 10 is precisely about answering one of these with the wrong sign.
#define OVRP_FAIL_INVALID_PARAM   (-1001)
#define OVRP_FAIL_NOT_INITIALIZED (-1002)
#define OVRP_FAIL_UNSUPPORTED     (-1004)
// ...and the one the real plugin uses for "there is nothing to hand you",
// which is not an error condition at the call site — ovrp_PollEvent2's own
// empty-queue path returns it (+0x26750), and the managed event loop reads any
// negative result as "stop polling this frame".
#define OVRP_FAIL_OPERATION       (-1006)

// Record a call. The hand-written implementations below call this themselves so
// that they stay in the report — the report is the M6 work list, and an entry
// point silently dropping off it once implemented is how the list stops matching
// what the guest actually does.
// KL_OVRP_TRACE=1: log the live call SEQUENCE, with a global ordinal and the
// caller's image+offset. The end-of-run work list prints TOTALS per name, and
// totals cannot answer the question the 1.40 display arc turns on — whether the
// guest re-enters the submit path per frame or entered it once and left. With
// the sequence, "BeginFrame4 once, then DestroyLayer" reads as a teardown;
// without it, it reads as a call count of 1.
//
// Everything the trace needs is already recorded, so this is a print and nothing
// else — no register capture and no reads of guest memory. Filtered to the
// frame/layer/display family, because the input pollers run thousands of times a
// frame and would bury it. Set KL_OVRP_TRACE=all for every entry point.
//
// `caller` is the guest's return address, and it arrives as a PARAMETER rather
// than being taken here: which __builtin_return_address level names the guest
// depends on whether this function was inlined into ovrp_hit, so computing it
// here would be a diagnostic that silently changes meaning with the optimiser.
// ovrp_hit takes it at a fixed level and is noinline for exactly that reason.
static void ovrp_trace(const char *name, void *caller) {
    static int on = -1;
    static uint64_t seq;
    if (on < 0) {
        const char *e = getenv("KL_OVRP_TRACE");
        on = !e ? 0 : (strcmp(e, "all") == 0 ? 2 : kl_env_on("KL_OVRP_TRACE", 0));
    }
    if (!on) return;
    if (on == 1) {
        // The prefixes carry "ovrp_" so a match against the guest's own name is
        // a prefix test: "ovrp_BeginFrame" matches BeginFrame4, and a bare
        // "Update" could never be the first bytes of "ovrp_Update3".
        static const char *const fam[] = {
            "ovrp_Update", "ovrp_WaitToBeginFrame", "ovrp_BeginFrame",
            "ovrp_EndFrame", "ovrp_EndEye", "ovrp_SetupDisplayObjects",
            "ovrp_SetupDistortionWindow", "ovrp_SetupLayer",
            "ovrp_CalculateLayerDesc", "ovrp_CalculateEyeLayerDesc",
            "ovrp_CalculateEyeViewportRect", "ovrp_CalculateEyePreviewRect",
            "ovrp_GetLayerTexture", "ovrp_GetEyeTexture",
            "ovrp_SetupEyeTexture", "ovrp_SetClientColorDesc",
            "ovrp_DestroyLayer", "ovrp_DestroyDistortionWindow",
            "ovrp_SetTiledMultiRes", "ovrp_GetTiledMultiRes",
            "ovrp_GetPredictedDisplayTime", "ovrp_SetFoveationEyeTracked",
            "ovrp_GetAppHasVrFocus", "ovrp_GetAppShouldQuit",
            "ovrp_Shutdown", "ovrp_Initialize",
        };
        int hit = 0;
        for (size_t i = 0; i < sizeof fam / sizeof *fam; i++)
            if (strncmp(name, fam[i], strlen(fam[i])) == 0) { hit = 1; break; }
        if (!hit) return;
    }
    size_t off = 0;
    const char *img = kl_addr_image(caller, &off);
    fprintf(stderr, "  [ovrp+] %5llu %s <- %s+0x%zx\n",
            (unsigned long long)seq++, name, img ? img : "?", off);
}

// noinline, and it must stay that way: level 0 is the entry-point handler that
// called us and level 1 is the GUEST that called the handler, which only holds
// while this function has a frame of its own. A handler must also not TAIL-call
// it — none do; every one records the hit and then goes on to answer.
__attribute__((noinline))
static void ovrp_hit(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    // -Wframe-address fires on any nonzero level. It is warning about exactly the
    // assumption stated above, which the noinline and the no-tail-call rule are
    // what make good; the fault reporter walks the same chain for the same reason.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-address"
    ovrp_trace(name, __builtin_return_address(1));
#pragma clang diagnostic pop
}

// M7 discovery: log each distinct argument value an input-family entry point
// is called with (node id, controller mask), once each, with the caller's
// image — names whose node/mask values the guest actually uses, and whether
// the poller is libunity or the game itself.
// The caller is part of the key, not just the argument: libunity's node loop
// polls every node every frame, so keying on (name, arg) alone suppresses the
// *game's* first call for a node libunity already asked about — which is
// exactly the call worth seeing (it tells apart "OVRInput reads hand poses"
// from "only the engine does"). Keyed on (name, arg) this read as "libil2cpp
// never asks for node 3", which was an artefact of the instrument.
static void ovrp_log_arg(const char *name, int arg, void *ra) {
    static struct { const char *name; int arg; void *ra; } seen[128];
    static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nseen; i++)
        if (seen[i].name == name && seen[i].arg == arg && seen[i].ra == ra) {
            pthread_mutex_unlock(&mu); return;
        }
    if (nseen < 128) {
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        fprintf(stderr, "  [ovrp] %s(arg=%d/0x%x) — first seen, called from %s+0x%zx\n",
                name, arg, arg, img ? img : "?", off);
        seen[nseen].name = name; seen[nseen].arg = arg;
        seen[nseen].ra = ra; nseen++;
    }
    pthread_mutex_unlock(&mu);
}

// Returns ovrpResult: 0 is success. Also covers the void entry points, where the
// return value is simply ignored by the caller.
static uint64_t klovrp_ok(const char *name) {
    ovrp_hit(name);
    return OVRP_SUCCESS;
}

// Returns ovrpBool: 1 is "yes". Used for the capability and state predicates that
// have to agree with the fact that we answered ovrp_Initialize5 with success.
static uint64_t klovrp_yes(const char *name) {
    ovrp_hit(name);
    return OVRP_TRUE;
}

// Returns ovrpBool: 0 is "no". For capabilities we genuinely do not have — the
// honest answer, and the one that keeps Unity off a path with nothing behind it.
static uint64_t klovrp_no(const char *name) {
    ovrp_hit(name);
    return 0;
}

// ---------------------------------------------------------------------------
// Has the guest initialised the plugin yet?
//
// This is STATE, not a capability, and it is the one predicate in this file
// whose right answer changes during a run. It used to sit in g_ovrp_bool_yes
// under "we answered ovrp_Initialize5 with success and stand behind it", which
// is true of every moment AFTER the guest calls Initialize — and Unity always
// does call it first (measured: libOculusXRPlugin issues ovrp_Initialize7
// seventh and ovrp_GetInitialized twelfth), so for four targets the constant
// and the state were the same number.
//
// UE4 asks in the opposite order, and it is not a poll — it is the GATE:
//
//     bool FOculusHMD::InitDevice()
//     {
//         if (ovrp_GetInitialized())
//             return true;              // "already created and present"
//         ...
//         LoadFromSettings();
//         if (!InitializeSession()) ...   // <- ovrp_Initialize5 lives in here
//         ...                             // <- and so does the whole eye layer
//     }
//
// (`libUE4.so+0x5c343f0`: `blr x8; cbz w0, +0x50; mov w20, #1` — the yes arm
// returns straight out.) So a constant yes told RE4 the HMD was already up,
// InitDevice returned success having built nothing, ovrp_Initialize5 was never
// called at all, and the engine walked on to ask that HMD for its eye render
// target — where `FOculusHMD::AllocateRenderTargetTexture` reads
// `EyeLayer_RenderThread->GetSwapChain()` with no null check and takes a
// SIGSEGV at 0x270 on a guest worker thread, four function calls and one
// thread from the answer that caused it.
//
// The state costs nothing to keep and is what the real plugin reports, so it is
// kept. Set by Initialize5/7, cleared by the Shutdown pair.
static int g_ovrp_initialized;

static uint64_t klovrp_GetInitialized(void) {
    ovrp_hit("ovrp_GetInitialized");
    return g_ovrp_initialized ? OVRP_TRUE : 0;
}

// Bring-up, and the decision recorded in PLANNING M6: we answer success and
// stand behind it rather than reporting a failure the guest would be right to
// believe. Both numbered forms exist because trap 10's whole shape is a
// numbered variant answering something different under a familiar name —
// Initialize7 is the ABI revision 1.40's libOculusXRPlugin actually calls (it
// resolves 5, 6 and 7 and uses the highest by construction), and RE4's UE4
// plugin calls 5. Same answer either way; the only thing that changed is that
// the answer is now RECORDED, so ovrp_GetInitialized can tell the truth.
static uint64_t klovrp_Initialize(const char *name) {
    ovrp_hit(name);
    g_ovrp_initialized = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_Initialize5(void) { return klovrp_Initialize("ovrp_Initialize5"); }
static uint64_t klovrp_Initialize7(void) { return klovrp_Initialize("ovrp_Initialize7"); }

// ...and the other end: the guest saying the plugin is down. ovrp_Shutdown is
// the ovrpBool wrapper (`!(result < 0)`) and answered yes before this change,
// so the answer is unchanged and only the record is new — a GetInitialized that
// still said yes afterwards would send a guest that re-enters InitDevice
// straight back into the fault above. ovrp_Shutdown2, the ovrpResult form, is
// deliberately NOT implemented here: nothing has ever called it, and an
// abort-by-name is how this project finds out that something does.
static uint64_t klovrp_Shutdown(void) {
    ovrp_hit("ovrp_Shutdown");
    g_ovrp_initialized = 0;
    return OVRP_TRUE;
}

// Who is driving. UE4's OculusHMD calls this immediately after Initialize5,
// with "UnrealEngine" and the engine version, and it is the first thing in this
// project ever to say which ENGINE a guest is — worth printing once rather than
// dropping, because a version string read off the running guest is a fact no
// unpacked tree states in one place.
//
// Recorded, not applied: there is nothing on this host that behaves differently
// for one engine than another. The shape is read out of RE4's own
// libOVRPlugin.so (+0x2fb7c) rather than inferred: ovrpResult, **-1001 if
// EITHER string is NULL** (two `cbz`es against a pre-loaded w19), -1002 with no
// singleton, and the third argument an ovrpBool the body passes straight
// through. The unsuffixed sibling at +0x36990 is the usual ovrpBool wrapper
// (`lsr w8, w0, #31; eor w0, w8, #1`) — trap 10's pair shape, so it gets its own
// handler rather than sharing this one.
static int64_t klovrp_engine_info(const char *name, const char *version,
                                  uint64_t is_editor) {
    if (!name || !version) return -1001;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ovrp] engine: %s %s%s\n", name, version,
                (is_editor & 1) ? " (editor)" : "");
    }
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetAppEngineInfo2(const char *name, const char *version,
                                         uint64_t is_editor) {
    ovrp_hit("ovrp_SetAppEngineInfo2");
    return (uint64_t)klovrp_engine_info(name, version, is_editor);
}
static uint64_t klovrp_SetAppEngineInfo(const char *name, const char *version,
                                        uint64_t is_editor) {
    ovrp_hit("ovrp_SetAppEngineInfo");
    return klovrp_engine_info(name, version, is_editor) < 0 ? 0 : OVRP_TRUE;
}

// The two per-frame "should the app tear itself down?" predicates, in their
// ovrpResult + out-param form. Their un-suffixed siblings already answer NO
// through g_ovrp_bool_no ("events that must never fire on a healthy run"), and
// this is the same answer through the other ABI: the real bodies (+0x2f924 and
// +0x2f9dc) NULL-check the pointer for -1001, take -1002 with no singleton, and
// otherwise store `result & 1` and return 0. Nothing here asks the app to quit
// and nothing recreates a distortion window, so both are 0 — and the out-param
// is what the caller reads, so a scalar-return implementation of these would be
// trap 10b: a `tbnz` on whatever was in the caller's stack slot.
static uint64_t klovrp_GetAppShouldQuit2(int *out) {
    ovrp_hit("ovrp_GetAppShouldQuit2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetAppShouldRecreateDistortionWindow2(int *out) {
    ovrp_hit("ovrp_GetAppShouldRecreateDistortionWindow2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// How many samples the DEVICE recommends for the eye render target. Same shape
// (+0x317f8: -1001 on NULL, -1002 with no singleton, else store and return 0),
// and the value is the Quest 2's, for the reason Build.MODEL says Quest 2 and
// ovrp_GetSystemHeadsetType2 answers 9: this is a property of the device we
// present, and answering something else here while claiming that device
// everywhere else is the display-panel group answer disagreeing with itself.
//
// It is a RECOMMENDATION, not a constraint — UE4 consults it only when the
// project asks for the device default — so the sample count the eye layer is
// actually built with still arrives in ovrp_CalculateEyeLayerDesc2 and is the
// guest's own number. KL_OVRP_MSAA overrides it, because the first thing to try
// if a multisampled eye layer turns out to be a problem is 1, and that is a
// measurement rather than a redefinition of the headset.
static uint64_t klovrp_GetSystemRecommendedMSAALevel2(int *out) {
    ovrp_hit("ovrp_GetSystemRecommendedMSAALevel2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (int)kl_env_int("KL_OVRP_MSAA", 4);
    return OVRP_SUCCESS;
}

// How long the GPU took on the last frame, in seconds. We do not measure it,
// and the failure is the honest answer: this is fed to adaptive resolution, so
// a fabricated 0 reads as "the GPU is idle" and is an instruction to render
// more. The real plugin (+0x31a84) returns the backend's own negative here when
// there is no timer, so a failing return is a path the guest already has.
//
// The out-param IS written, which the real one's failure path does not do, and
// that is deliberate: on a Quest this call succeeds, so a caller that reads the
// float without checking the result is a bug no real device would ever expose
// (trap 10b, from the other side). Zero is what such a caller gets instead of
// whatever was in its stack slot; a caller that checks never sees it.
static uint64_t klovrp_GetGPUFrameTime(float *out) {
    ovrp_hit("ovrp_GetGPUFrameTime");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0.0f;
    return OVRP_FAIL_UNSUPPORTED;
}

// Mixed-reality capture, refused. `ovrp_GetMixedRealityInitialized` beside this
// has always answered false, and the two have to agree: the real plugin's
// GetMixedRealityInitialized (+0x34b38) is a plain read of the byte
// InitializeMixedReality sets, so answering success here and false there
// describes a plugin that initialised MRC and then forgot. There is no capture
// camera on this host and no composite path behind it.
static uint64_t klovrp_InitializeMixedReality(void) {
    ovrp_hit("ovrp_InitializeMixedReality");
    return OVRP_FAIL_UNSUPPORTED;
}

// Chromatic-aberration correction. Both halves are `return GetInitialized()`
// stubs in the real plugin — the correction moved compositor-side and the entry
// points survive for ABI only — so they read the same flag rather than a
// constant, which is the real library's own body. The setter IGNORES its
// argument; that is the real one's behaviour too (SUPERHOT's libOVRPlugin.so
// +0x29f4c is `bl ovrp_GetInitialized; cmp w0, #0; cset w0, ne; ret`), and
// answering ovrpResult's 0 here would read to the caller as false, i.e. trap 10.
static uint64_t klovrp_GetAppChromaticCorrection(void) {
    ovrp_hit("ovrp_GetAppChromaticCorrection");
    return g_ovrp_initialized ? OVRP_TRUE : 0;
}
static uint64_t klovrp_SetAppChromaticCorrection(uint64_t on) {
    (void)on;
    ovrp_hit("ovrp_SetAppChromaticCorrection");
    return g_ovrp_initialized ? OVRP_TRUE : 0;
}

// ---------------------------------------------------------------------------
// The synthetic headset
//
// One coherent device description, for the reason the display group in kl_jni.c
// and the GLES capability set in kl_egl.c are each answered as a whole: Unity
// asks these in several places and cross-checks the answers, so they have to
// describe the same hardware. What is described is the Quest 2 we already claim
// to be through Build.MODEL — not the Vision Pro underneath — because every
// Oculus branch in the guest is written against that.
#define KL_OVRP_EYE_W    (1832*1.25)        // Quest 2 per-eye recommended render target
#define KL_OVRP_EYE_H    (1920*1.25)
#define KL_OVRP_REFRESH  72.0f       // Quest 2 default display frequency

// The version string Unity parses to gate optional features behind "is the
// plugin at least 1.x". Taken from the OVRPlugin actually shipped in this APK
// (strings(1) reports 1.60.0; the package is ovrplugin-android-universal:28.0.0)
// rather than picked: the guest's managed side was compiled against that plugin,
// so any other number invites it down a path its own C# does not match.
#define KLOVRP_VERSION "1.60.0"

static const char *klovrp_GetVersion(void) {
    ovrp_hit("ovrp_GetVersion");
    return KLOVRP_VERSION;
}

// **The 2-form is not the same function with a different name.** Real
// ovrp_GetVersion2 (+0x16d970) is `ovrpResult ovrp_GetVersion2(const char **)`:
// it `cbz x0` -> -1001, stores the string through `[x0]`, and returns 0. The
// un-suffixed one at +0x175ab0 is a WRAPPER around it — a stack slot, a call,
// and `csel x0, xzr, x8, lt` — which is what makes that one a scalar return.
// This is exactly the convention klovrp_GetSystemProductName2 already documents
// two hundred lines down, and this entry point was written the other way.
//
// Answering in x0 costs nothing where the CALLER also ignores it — libunity and
// libOculusXRPlugin go through the C# `ovrp_GetVersion`, so it read correctly
// for the whole project. **1.40's libhaptics_sdk calls the 2-form directly**,
// from native Rust, and then `strlen`s what it believes we wrote: an
// uninitialised stack slot, in practice NULL. That is a SIGSEGV at 0x0 inside
// `_platform_strlen` with one guest frame above it and nothing naming haptics,
// the version, or this function — and it only became reachable once the PCM
// entry points stopped aborting first.
static uint64_t klovrp_GetVersion2(const char **out) {
    ovrp_hit("ovrp_GetVersion2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = KLOVRP_VERSION;
    return OVRP_SUCCESS;
}

// ...and the SDK-version pair, which is the same two functions in the same
// relationship — `ovrp_GetNativeSDKVersion` (+0x26c18 in SUPERHOT's plugin, a
// 44-byte wrapper, no argument refused) around
// `ovrp_GetNativeSDKVersion2(const char **)` (+0x22b4c, `cbz x0` -> -1001),
// confirmed with tools/ovrp_abi.py against the real library rather than assumed
// from the naming. Writing this pair the other way round is trap 10b exactly.
//
// What it reports is the version of the NATIVE SDK under the plugin — VrApi on
// a Quest. There is none here (PLANNING §3.1: libvrapi.so is never loaded), so
// the honest answer is the one thing that is true, our own plugin version, and
// it is the same string ovrp_GetVersion answers so the two cannot describe
// different plugins. Unity's own use of it is diagnostic — OVRManager logs
// "OVRPlugin v{0}, SDK v{1}" — and SUPERHOT's C# reached it during its startup
// logging, where an unimplemented entry point is an abort.
static const char *klovrp_GetNativeSDKVersion(void) {
    ovrp_hit("ovrp_GetNativeSDKVersion");
    return KLOVRP_VERSION;
}

static uint64_t klovrp_GetNativeSDKVersion2(const char **out) {
    ovrp_hit("ovrp_GetNativeSDKVersion2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = KLOVRP_VERSION;
    return OVRP_SUCCESS;
}

// ovrpSystemHeadset. The value is read out of the guest's own IL2CPP metadata
// rather than from an OVRPlugin header we do not have: global-metadata.dat lists
// the enum as Oculus_Quest, Oculus_Quest_2, Placeholder_10 .. Placeholder_14,
// Rift_DK1 ... — and the placeholders name their own values, which pins
// Oculus_Quest_2 at 9 (and, via PC_Placeholder_4103, Rift_DK1 at 0x1000).
//
// Quest 2 for the same reason Build.MODEL says Quest 2: it is the device this
// title is written for, and the answer has to agree with the one JNI already gave.
#define OVRP_HEADSET_OCULUS_QUEST_2 9
static uint64_t klovrp_GetSystemHeadsetType(void) {
    ovrp_hit("ovrp_GetSystemHeadsetType");
    return OVRP_HEADSET_OCULUS_QUEST_2;
}

static uint64_t klovrp_GetSystemHeadsetType2(int *out) {
    ovrp_hit("ovrp_GetSystemHeadsetType2");
    // The 1.40 ABI for the headset-type query: ovrpResult + int* out-param,
    // NOT the scalar-returning OVRP_0_1_x form — the real plugin writes the
    // enum through the pointer (0x16dd00) and returns -1001 for a NULL one.
    // Same value as the unsuffixed form: Quest 2 is what we claim everywhere
    // else, and our PreInitialize3/Initialize7 answers mean the preinit gate
    // the real plugin checks first is already satisfied, so only the NULL
    // guard is reachable.
    if (!out) return -1001;
    *out = OVRP_HEADSET_OCULUS_QUEST_2;
    return OVRP_SUCCESS;
}

// Real plugin is ovrpResult + int* out-param (0x172530), NOT a scalar return;
// it NULL-checks x0 and writes the count. Nothing has ever been recentered on
// this host, so the honest answer is 0.
static uint64_t klovrp_GetLocalTrackingSpaceRecenterCount(int *out) {
    ovrp_hit("ovrp_GetLocalTrackingSpaceRecenterCount");
    if (!out) return -1001;
    *out = 0;
    return OVRP_SUCCESS;
}

// Eye-tracked foveation is a Quest Pro feature; we present a Quest 2, and our
// foveation is the fixed/VRR kind (ovrp_SetTiledMultiResDynamic family), not
// one that follows the gaze — so "not supported" is the honest answer, and the
// guest then uses the fixed foveation path. Real plugin is ovrpResult + int*
// out (0x1719a0).
static uint64_t klovrp_GetFoveationEyeTrackedSupported(int *out) {
    ovrp_hit("ovrp_GetFoveationEyeTrackedSupported");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

// Multiview — "can the two eyes be two ARRAY LAYERS of one texture, rendered in
// one pass?" — is the single answer that decides the guest's whole stereo
// rendering mode, and it is ours to give.
//
// **It is OFF by default and that is a property of the GL gateway, not of the
// hardware.** ANGLE-on-Metal has no multiview here, so every GLES guest must be
// told no, and the un-suffixed scalar form in g_ovrp_bool_no says the same
// thing. Answering yes to a GLES guest is an eye texture it renders half of.
//
// On the VULKAN path the answer is YES, because MoltenVK implements
// `VK_KHR_multiview` and kl_vulkan.c hands back a two-layer image.
//
// **That is not a performance choice, it is what makes BONELAB render at all.**
// Answering no put Unity in MultiPass, and in MultiPass this title's SRP threw
// `ArgumentOutOfRangeException` once per frame before drawing anything — 856 in
// a 255-frame run, each followed by `XRSystem.ReleaseFrame() was not called!`,
// with an EMPTY stack trace because the build carries none. Nothing about the
// XR display was wrong: `XRSettings` reported `enabled=1`, the right
// 2208x2400 eye texture and a healthy `oculus display`. It was the RENDERING
// MODE — a Quest title ships its shader variants and its renderer for Single
// Pass Instanced, and MultiPass is a path it may never have run. Flipping this
// one answer took the exception count to 0 and put the loading screen in the
// eye images on the first try.
//
// It only takes effect once the guest has actually brought a Vulkan device up
// through us, so a GLES guest is unaffected: ANGLE-on-Metal has no multiview
// here, and telling one yes is an eye texture it renders half of.
// `KL_OVRP_MULTIVIEW=0` is the A/B, and it restores the failing configuration
// exactly — which is the only way to see that exception again.
//
// The two forms must not disagree: ovrp_GetEyeTextureArraySupported is the same
// question and reads this.
int kl_ovrp_multiview(void) {
    if (!kl_vulkan_guest_active()) return 0;
    return kl_env_on("KL_OVRP_MULTIVIEW", 1);
}

// The 2-form of the multiview capability is ovrpResult + int* out (real
// 0x171090), unlike the scalar-bool un-suffixed form already refused in
// g_ovrp_bool_no.
// Both 2-forms `cbz x0 -> -1001` before doing anything (read at +0x12a1b0 and
// +0x12a210), so a NULL out is INVALID_PARAM rather than a success that wrote
// nothing — `make ovrpabi` checks exactly this against the real library.
static uint64_t klovrp_GetSystemMultiViewSupported2(int *out) {
    ovrp_hit("ovrp_GetSystemMultiViewSupported2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = kl_ovrp_multiview();
    return OVRP_SUCCESS;
}

// The same question under its other name — the provider asks this one to decide
// whether the eye textures can be a 2-slice array. Answering it separately from
// the multiview form is how a guest ends up told it may render single-pass into
// storage that has one layer.
static uint64_t klovrp_GetEyeTextureArraySupported2(int *out) {
    ovrp_hit("ovrp_GetEyeTextureArraySupported2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = kl_ovrp_multiview();
    return OVRP_SUCCESS;
}

// The un-suffixed pair. Both are plain `bool` WRAPPERS around the 2-forms in the
// real plugin — `bl ...2; csel w0, wzr, w8, lt`, i.e. the out value when the
// Result was non-negative and false otherwise (read at +0x130bc0 / +0x130b60) —
// so they are the scalar-bool shape and must answer whatever the 2-forms do.
// They were a flat "no" in g_ovrp_bool_no until the Vulkan path made the answer
// conditional, and a constant beside a variable is trap 10's neighbour: the
// guest asks twice under two names and acts on whichever it read last.
static uint64_t klovrp_GetEyeTextureArraySupported(void) {
    ovrp_hit("ovrp_GetEyeTextureArraySupported");
    return (uint64_t)kl_ovrp_multiview();
}
static uint64_t klovrp_GetSystemMultiViewSupported(void) {
    ovrp_hit("ovrp_GetSystemMultiViewSupported");
    return (uint64_t)kl_ovrp_multiview();
}

// Guardian boundary: there is no boundary system on this host, so the
// configured flag is false — same stance as the Geometry2/Visible forms. Real
// plugin is ovrpResult + int* out (0x171170).
static uint64_t klovrp_GetBoundaryConfigured2(int *out) {
    ovrp_hit("ovrp_GetBoundaryConfigured2");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

// Gate predicates whose 2-forms are ovrpResult + int* out (the un-suffixed
// forms are the OVRP_0_1_x scalar/mask shapes already answered in
// g_ovrp_bool_yes). Each writes TRUE because the un-suffixed sibling already
// says yes and the two must not disagree: VR focus (which gates whether Unity
// renders at all), the head present, and the node present.
static uint64_t klovrp_GetAppHasVrFocus2(int *out) {
    ovrp_hit("ovrp_GetAppHasVrFocus2");
    if (out) *out = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetUserPresent2(int *out) {
    ovrp_hit("ovrp_GetUserPresent2");
    if (out) *out = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetNodePresent2(int node, int *out) {
    ovrp_hit("ovrp_GetNodePresent2");
    // node comes FIRST (real 0x16f450: w0 = ovrpNode, x1 = int* out) — the
    // last "write a bool" shape this guest has that is NOT a single
    // pointer. Any node a caller names is one we report; the value is written
    // for the callers that read it, since at least one (OculusSystem::
    // GetNodePresent, libOculusXRPlugin+0xf264) only checks the return.
    (void)node;
    if (out) *out = 1;
    return OVRP_SUCCESS;
}

// Another node-first predicate, real 0x16f4b0 is the same (int node, int *out)
// shape as GetNodePresent2 — w0=node, x1=out. Orientation IS tracked for
// every node we report poses for.
static uint64_t klovrp_GetNodeOrientationTracked2(int node, int *out) {
    ovrp_hit("ovrp_GetNodeOrientationTracked2");
    (void)node;
    if (out) *out = 1;
    return OVRP_SUCCESS;
}

// The real plugin never fills this one at all — 0x1706d0 NULL-checks its out,
// then returns -1004 (unsupported) with *out untouched. We answer SUCCESS with
// a nominal 20 C instead: there is no temperature reading here, but an error
// return is only safely handled if the guest happens to tolerate it, and a
// plausible static nominal is harmless telemetry either way (leaving *out
// untouched would read as 0 C, a value nobody sent).
//
// The nominal is a seam rather than a literal for the same reason the level is:
// UE4's BatteryReceiver dispatches a temperature alongside the level and the
// charging status, so two constants would be two temperatures for one battery.
int kl_ovrp_battery_temperature(void) { return 20; }

static uint64_t klovrp_GetSystemBatteryTemperature2(int *out) {
    ovrp_hit("ovrp_GetSystemBatteryTemperature2");
    if (!out) return -1001;
    *out = kl_ovrp_battery_temperature();   // nominal °C — no sensor is presented
    return OVRP_SUCCESS;
}

// Battery level, through the kl_ovrp_battery_level seam so this cannot drift
// from what the Java BatteryManager answers or from what a visionOS frontend
// pushes off UIDevice. The real plugin's 2-form is another never-writing stub
// (0x1706a0 returns -1004); we answer success with the shared level instead.
static uint64_t klovrp_GetSystemBatteryLevel2(int *out) {
    ovrp_hit("ovrp_GetSystemBatteryLevel2");
    if (!out) return -1001;
    *out = kl_ovrp_battery_level();
    return OVRP_SUCCESS;
}

// No power-saving mode is active. Real plugin is ovrpResult + int* out
// (0x1703f0).
static uint64_t klovrp_GetSystemPowerSavingMode2(int *out) {
    ovrp_hit("ovrp_GetSystemPowerSavingMode2");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

// Recenter is an event that must never fire on a healthy run, and no frontend
// has asked for one — the un-suffixed form answers 0 and this 2-form is the
// ovrpResult + int* out version of the same refusal (real 0x1708e0).
static uint64_t klovrp_GetAppShouldRecenter2(int *out) {
    ovrp_hit("ovrp_GetAppShouldRecenter2");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

// The time (seconds, monotonic) the guest is told the next frame will be shown,
// for its timewarp pose prediction. Real signature (0x171fd0) is
// ovrpResult + double* out — s0 is NOT the return, the value goes through the
// pointer. Answered through the predicted-display-time seam, which a visionOS
// frontend keeps in sync with the drawable presentation time.
static uint64_t klovrp_GetPredictedDisplayTime(int node, double *out) {
    ovrp_hit("ovrp_GetPredictedDisplayTime");
    (void)node;
    if (!out) return -1001;
    *out = kl_ovrp_predicted_display_time();
    return OVRP_SUCCESS;
}

// libunity's OculusVRDevice::Initialize calls this for the product name and,
// if the return is non-NULL, strlens and hashes it (guest code at 0x9bb538 —
// NULL is explicitly tolerated). A static string is therefore both safe and
// required if we want the name answered at all; "Oculus Quest 2" is what the
// real plugin returns on the device we are presenting as everywhere else.
static const char *klovrp_GetSystemProductName(void) {
    ovrp_hit("ovrp_GetSystemProductName");
    return "Oculus Quest 2";
}

// The 2-form of the same answer: ovrpResult + const char** out — real
// 0x170700 writes the string POINTER through [out], it is not a scalar
// return. Same "Oculus Quest 2" as the un-suffixed form; the two product-
// name surfaces must never disagree, and a static literal is never freed.
static uint64_t klovrp_GetSystemProductName2(const char **out) {
    ovrp_hit("ovrp_GetSystemProductName2");
    if (!out) return -1001;
    *out = "Oculus Quest 2";
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// The libunity steady-state contract
//
// Everything below exists because guest libunity.so (2019.4.28f1, legacy VR)
// calls it through the dlsym'd Oculus table, with the argument shapes and
// return checks recovered from its own machine code — not invented from an
// OVRPlugin header. Offsets cited in the comments are guest libunity
// addresses; the recovery story is in PLANNING.md §M6.

// float return: s0, not x0 — this is why it cannot share klovrp_yes.
// Stored into Unity's VR timing config (0x9bce28). No division by it in the
// Oculus path, but 0.0 would poison Unity-side pacing math; 72 is the Quest 2
// default and agrees with the device we describe.
// The display frequency we report. Quest 2's 72 Hz by default — the device we
// claim to be everywhere else — until a frontend measures the real one and
// pushes it through kl_ovrp_set_display_frequency. Unity reads this once,
// early, and stores it into its VR timing config, so the push has to happen
// before the guest boots; on visionOS that is what the compositor's priming
// pass is for.
static float g_display_hz = KL_OVRP_REFRESH;

void kl_ovrp_set_display_frequency(float hz) {
    // A display frequency is a divisor in Unity's pacing math and a period
    // everywhere else. Anything outside the range a headset can actually run at
    // is a measurement that went wrong, and passing it on turns one bad number
    // into a frame loop that never settles.
    if (!(hz >= 30.0f) || !(hz <= 240.0f)) return;
    g_display_hz = hz;
}

// KL_DISPLAY_HZ=<hz> forces it. On the host there is no headset to measure, so
// the 72 above is the Quest-2 fiction the rest of the device description keeps
// up; this is how a host run says "pretend the panel runs at what the thing on
// the other end of the wire is asking for". Read once, and through the same
// 30..240 sanity check as the setter, so a typo cannot poison pacing math.
float kl_ovrp_display_frequency(void) {
    static int checked;
    if (!checked) {
        checked = 1;
        const char *e = getenv("KL_DISPLAY_HZ");
        if (e && *e) {
            float hz = strtof(e, NULL);
            if (hz >= 30.0f && hz <= 240.0f) {
                fprintf(stderr, "  [ovrp] display frequency forced to %.1f Hz "
                                "by KL_DISPLAY_HZ\n", (double)hz);
                g_display_hz = hz;
            }
        }
    }
    return g_display_hz;
}

// ---- battery telemetry seam ----
// One source of truth for the battery, routed through by the OVRPlugin query
// (GetSystemBatteryLevel2), the Java BatteryManager answer (kl_jni.c), and —
// on device — a frontend pushing UIDevice's real reading. Sanity checks mirror
// the display-frequency setter: a level outside 0..100 is a bad measurement,
// and passing it on would make every consumer answer nonsense.
static int g_battery_level = 95;       // Quest-2 fiction, like the display: 72 Hz
static int g_battery_charging;

void kl_ovrp_set_battery_level(int level) {
    g_battery_level = level < 0 ? 0 : (level > 100 ? 100 : level);
}
int kl_ovrp_battery_level(void) {
    static int checked;
    if (!checked) {
        checked = 1;
        const char *e = getenv("KL_BATTERY_LEVEL");
        if (e && *e) kl_ovrp_set_battery_level(atoi(e));
    }
    return g_battery_level;
}
void kl_ovrp_set_battery_charging(int charging) { g_battery_charging = !!charging; }
int kl_ovrp_battery_charging(void) {
    static int checked;
    if (!checked) {
        checked = 1;
        const char *e = getenv("KL_BATTERY_CHARGING");
        if (e && *e) g_battery_charging = atoi(e) != 0;
    }
    return g_battery_charging;
}

// ---- predicted display time seam ----
// 0 means "nothing pushed; answer the monotonic present". A visionOS frontend
// overwrites it with the real drawable presentation time, and the guest's
// timewarp then predicts the pose using a timestamp that matches when the
// frame actually scans out. Sanity window: a presentation time within 0..1e9 s
// is a plausible timestamp, anything else is a bad measurement and discarded.
static double g_predicted_display;
void kl_ovrp_set_predicted_display_time(double t) {
    if (t > 0.0 && t < 1e9) g_predicted_display = t;
}
double kl_ovrp_predicted_display_time(void) {
    if (g_predicted_display > 0.0) return g_predicted_display;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float klovrp_GetSystemDisplayFrequency(void) {
    ovrp_hit("ovrp_GetSystemDisplayFrequency");
    return kl_ovrp_display_frequency();
}

// The ...2 shape: ovrpResult with a float out-param, where the un-suffixed form
// returns the float directly. 1.40's libOculusXRPlugin calls THIS one, from
// OculusSystem::GetDisplayRefreshRate, which pre-zeroes its local and ignores
// the result — so an unimplemented answer here is a refresh rate of 0.0 handed
// to Unity's frame pacer rather than an error anyone reports.
//
// The real 0x170460 NULL-checks the out pointer (-1001), and on success returns
// 0 having written s0 through it.
static uint64_t klovrp_GetSystemDisplayFrequency2(float *out) {
    ovrp_hit("ovrp_GetSystemDisplayFrequency2");
    if (!out) return -1001;
    *out = kl_ovrp_display_frequency();
    return OVRP_SUCCESS;
}

// The rate the guest may ASK for. We do not own a display here — the one rate we
// can actually present at is the one the compositor measured (or KL_DISPLAY_HZ,
// or the Quest-2 default on a host run) — so the honest answer is a headset with
// exactly one available mode: agree when asked for the rate we already run at,
// and refuse otherwise rather than accept a rate we would not deliver.
//
// This is what a 72 Hz Quest 2 does when a title asks for 90, and Beat Saber has
// that path: it reads the available list first, does not find what it wants, and
// logs "Could not set display frequency of 90" without calling the setter at
// all. A refusal here is therefore the same answer by a second route, and
// accepting would be the invented one — the list is a promise (see
// klovrp_GetSystemDisplayAvailableFrequencies).
static uint64_t klovrp_SetSystemDisplayFrequency(float hz) {
    ovrp_hit("ovrp_SetSystemDisplayFrequency");
    float have = kl_ovrp_display_frequency();
    if (hz == have) return OVRP_SUCCESS;
    fprintf(stderr, "  [ovrp] SetSystemDisplayFrequency(%.1f) refused — this "
                    "display runs at %.1f Hz and cannot switch\n",
            (double)hz, (double)have);
    return -1001;                     // ovrpFailure_InvalidParameter
}

// The per-eye render target size. Quest 2's recommendation until a frontend
// measures the display it is actually running on — see kl_ovrp.h.
static int g_eye_w = (int)KL_OVRP_EYE_W;
static int g_eye_h = (int)KL_OVRP_EYE_H;

void kl_ovrp_set_eye_texture_size(int w, int h) {
    // A degenerate size is a measurement that went wrong, and it would reach
    // Unity as a swapchain allocation. Keep the Quest default instead: the
    // wrong resolution renders, a zero one does not.
    if (w <= 0 || h <= 0) return;
    const int req_w = w, req_h = h;

    // Unity applies its own resolution scale on top of whatever we say
    // (measured 1.2x), and allocates stages x eyes of RGBA16F at the result. A
    // display with a much larger logical resolution than a Quest therefore
    // turns straight into hundreds of MiB and into fill cost, so what the
    // display asks for is scaled and capped rather than taken on trust.
    float scale = kl_env_float("KL_OVRP_EYE_SCALE", 1.0f);
    if (scale > 0.05f && scale <= 4.0f) {
        w = (int)((float)w * scale + 0.5f);
        h = (int)((float)h * scale + 0.5f);
    }
    int cap = kl_env_int("KL_OVRP_EYE_MAX", 3072);
    if (cap > 0 && (w > cap || h > cap)) {
        // Preserve the aspect ratio: clamping the axes independently would
        // change the shape of the picture, and the frustum tangents pushed
        // alongside this would then disagree with it.
        float k = (float)cap / (float)(w > h ? w : h);
        w = (int)((float)w * k + 0.5f);
        h = (int)((float)h * k + 0.5f);
    }
    if (w == g_eye_w && h == g_eye_h) return;

    // Logged every change, not once: each one costs the guest a swapchain
    // rebuild, and how many of those happen is exactly the question §12.21 was
    // about. The MiB figure is the whole swapchain Unity will hold.
    double mib = (double)w * h * 8.0 * 2.0 * kl_ovrp_stage_count() / (1024 * 1024);
    fprintf(stderr, "  [ovrp] eye texture size %dx%d -> %dx%d; the display asked "
                    "for %dx%d (scale %.2f, cap %d). %.0f MiB of swapchain, "
                    "before Unity's own ~1.2x\n",
            g_eye_w, g_eye_h, w, h, req_w, req_h, scale, cap, mib);
    g_eye_w = w;
    g_eye_h = h;
}

void kl_ovrp_eye_texture_size(int *w, int *h) {
    if (w) *w = g_eye_w;
    if (h) *h = g_eye_h;
}

// Packed return: width in the low 32 bits, height in the high (0x9bce58).
static uint64_t klovrp_GetEyeTextureSize(void) {
    ovrp_hit("ovrp_GetEyeTextureSize");
    return (uint64_t)(uint32_t)g_eye_w | ((uint64_t)(uint32_t)g_eye_h << 32);
}

int kl_ovrp_stage_count(void);

static uint64_t klovrp_GetEyeTextureStageCount(void) {
    ovrp_hit("ovrp_GetEyeTextureStageCount");
    return (uint64_t)kl_ovrp_stage_count();
}

// libunity maps 3->22, 2->2, anything else->4 (0x9bcf40), so any value is
// survivable; 2 keeps it on the mapping the real plugin produces.
static uint64_t klovrp_GetDesiredEyeTextureFormat(void) {
    ovrp_hit("ovrp_GetDesiredEyeTextureFormat");
    return 2;
}

// The frustum we tell the guest to render with, per eye: left, right, top,
// bottom tangents, all positive (cp_view_get_tangents order — see kl_ovrp.h).
// 1.0 everywhere is the coherent symmetric 90-degree frustum every host run has
// used; a frontend that knows the display's real field of view overwrites it
// through kl_ovrp_set_eye_frustum.
static float g_eye_tan[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};

void kl_ovrp_set_eye_frustum(int eye, float left, float right, float top, float bottom) {
    if ((unsigned)eye > 1) return;
    // A zero or negative tangent is not a narrow frustum, it is a degenerate
    // one — libunity divides by their max for the aspect. Refuse rather than
    // pass it on; the default stays, which is at worst the wrong field of view
    // instead of a division by zero four layers down.
    if (!(left > 0) || !(right > 0) || !(top > 0) || !(bottom > 0)) return;
    g_eye_tan[eye][0] = left;  g_eye_tan[eye][1] = right;
    g_eye_tan[eye][2] = top;   g_eye_tan[eye][3] = bottom;
}

// The two knobs over g_eye_tan, both applied on the READ side.
//
// KL_OVRP_UNIFY_FRUSTUM — hand both eyes the union of the cones. Default 0.
// Oculus symmetric projection means the PLUGIN computes this union itself:
// libOculusXRPlugin takes max(L0,L1)/max(R0,R1), renders both eyes with it,
// widens its own eye texture by the ratio to preserve angular density, and
// submits a per-eye ovrpLayerSubmit.ViewportRect naming each eye's sub-rect
// (BONELAB, canted display forced on host: desc 2290x2400 becomes a 2880x2400
// layer, eye 0 at 0,0 2271x2400, eye 1 at 609,0 2271x2400). Unifying here
// applies that arithmetic a layer too early: the guest then computes a widening
// ratio of 1, keeps the original width, and each eye's cone lands on 78.9% of
// the pixels it should have. The per-eye rects come back full-width, so nothing
// downstream can detect it. The composite honours the submitted rects instead
// (kl_reproject.h, grid_per_eye). Set 1 only for a guest that collapses the
// cones WITHOUT submitting per-eye rects; that case is unservable otherwise.
//
// KL_OVRP_EYE_TAN — push a canted pair of cones on a run with no display to
// measure one from, which on the host is every run. The default is the
// symmetric {1,1,1,1}, and a symmetric display makes every asymmetry question
// unaskable: the union equals each eye's own cone, a guest that collapses the
// two is indistinguishable from one that honours them, and any per-eye viewport
// comes out full width. That failure family is invisible on the host.
//
//   KL_OVRP_EYE_TAN=vision     Vision Pro's, measured off the drawable: eye 0
//                              l=1.73205 r=1.0 t=1.0 b=1.19175, eye 1 mirrored
//                              (tan 60/45 out/in, tan 45/50 up/down)
//   KL_OVRP_EYE_TAN=l,r,t,b    that pair from eye 0's cone, mirrored for eye 1
//
// Read side, not write side: the compositor pushes the display's cones from
// primeDisplay, before the guest has brought Vulkan up, so at write time
// kl_ovrp_multiview() is still false and a mode decided later could never be
// consulted. Every consumer routes through here — what the guest is told
// (ovrp_GetNodeFrustum2, the eye layer desc), what kl_ovrp_eye_view reports,
// and the frame record the composite builds its quad from. That last one must
// AGREE with what the guest rendered; reading g_eye_tan directly is the bug.
static const float *klovrp_forced_tan(void) {
    static float t[2][4];
    static int state;                        // 0 unread, 1 none, 2 forced
    if (!state) {
        const char *s = kl_env_str("KL_OVRP_EYE_TAN", NULL);
        float l = 0, r = 0, tp = 0, b = 0;
        if (s && (*s == 'v' || *s == 'V')) {
            l = 1.73205f; r = 1.0f; tp = 1.0f; b = 1.19175f;
        } else if (s && *s) {
            if (sscanf(s, "%f,%f,%f,%f", &l, &r, &tp, &b) != 4) l = 0;
        }
        if (l > 0 && r > 0 && tp > 0 && b > 0) {
            t[0][0] = l;  t[0][1] = r;  t[0][2] = tp; t[0][3] = b;
            t[1][0] = r;  t[1][1] = l;  t[1][2] = tp; t[1][3] = b;
            fprintf(stderr, "  [ovrp] KL_OVRP_EYE_TAN: forcing a CANTED display "
                            "— eye 0 l=%.5f r=%.5f t=%.5f b=%.5f, eye 1 mirrored\n",
                    (double)l, (double)r, (double)tp, (double)b);
            state = 2;
        } else {
            if (s && *s)
                fprintf(stderr, "  [ovrp] KL_OVRP_EYE_TAN=%s is not `vision` or "
                                "four positive tangents l,r,t,b — ignored\n", s);
            state = 1;
        }
    }
    return state == 2 ? &t[0][0] : NULL;
}

static const float *klovrp_eye_tan(int eye) {
    // Written by whichever thread reads first and rewritten with identical
    // values by any other — the inputs are the display's, fixed for the run.
    static float u[2][4];
    if ((unsigned)eye > 1) eye = 0;
    const float *forced = klovrp_forced_tan();
    const float (*src)[4] = forced ? (const float (*)[4])forced
                                   : (const float (*)[4])g_eye_tan;
    // Read once: this is on the frame path (every ovrp_GetNodeFrustum2, every
    // layer desc, every frame record) and the answer cannot change mid-run.
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_UNIFY_FRUSTUM", 0);
    if (!on) return src[eye];
    for (int i = 0; i < 4; i++)
        u[0][i] = u[1][i] = src[0][i] > src[1][i] ? src[0][i] : src[1][i];
    // Said once, and said with the numbers: a frustum that is silently widened
    // is indistinguishable from one that was measured that way, and the eye
    // texture's pixel budget moved with it.
    static int said;
    if (!said++)
        fprintf(stderr, "  [ovrp] KL_OVRP_UNIFY_FRUSTUM=1: both eyes told ONE "
                        "frustum l=%.4f r=%.4f t=%.4f b=%.4f (the union of the "
                        "two cones — the guest then renders at the union's "
                        "angular density and submits full-width per-eye rects)\n",
                (double)u[0][0], (double)u[0][1], (double)u[0][2], (double)u[0][3]);
    return u[eye];
}

// The head->eye offsets, head-local metres, and the guest's whole source of
// stereo separation — see kl_ovrp.h. Zero is the historical host behaviour and
// stays the default; the visionOS compositor pushes the display's own numbers.
static float g_eye_off[2][3];

void kl_ovrp_set_eye_offset(int eye, float x, float y, float z) {
    if ((unsigned)eye > 1) return;
    g_eye_off[eye][0] = x; g_eye_off[eye][1] = y; g_eye_off[eye][2] = z;
}

// The eye's own ROTATION — the cant — which this seam used to drop on the floor.
//
// Vision Pro's displays are angled outward, so device_from_view is not a pure
// translation: each eye is turned, oppositely, and its frustum tangents are
// expressed in that turned frame (measured: l=1.7321 r=1.0000 for the left eye,
// mirrored for the right — tan 60 out, tan 45 in). We were handing the guest
// the turned frame's *tangents* while telling it the eye pointed straight
// ahead, so it rendered the right shape of cone in the wrong direction.
//
// The composite then placed that picture and viewed it through the real canted
// eye (kl_reproject.c's view_rot), which is the correct thing to do with the
// picture it was given — and the result on screen is each eye's image rotated
// by the cant, in opposite directions. Opposite per-eye rotation is the one
// error the visual system cannot merge: it is seen as **two images**, not as
// blur, which is what "doubling during head turns" was.
//
// Identity by default, so a host run and every headless test are unchanged.
// KL_OVRP_EYE_CANT=0 restores the dropped-cant behaviour as the A/B.
static float g_eye_rot[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };

void kl_ovrp_set_eye_rotation(int eye, float qx, float qy, float qz, float qw) {
    if ((unsigned)eye > 1) return;
    // A zero quaternion is not a rotation; treat it as "none" rather than
    // collapsing the eye's basis to nothing.
    if (!(qx * qx + qy * qy + qz * qz + qw * qw > 1e-6f)) return;
    g_eye_rot[eye][0] = qx; g_eye_rot[eye][1] = qy;
    g_eye_rot[eye][2] = qz; g_eye_rot[eye][3] = qw;
}

static int klovrp_eye_cant(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_EYE_CANT", 0);
    }
    return on;
}


// KL_OVRP_IPD=<metres>: force a symmetric separation, ignoring whatever the
// frontend pushed. The A/B for "is the compositor's number the wrong one" —
// and, with no frontend at all, the only way to get stereo out of a host run.
static void klovrp_eye_offset(int eye, float *ox, float *oy, float *oz) {
    static float forced = -1.0f;
    if (forced < 0.0f) {
        float v = kl_env_float("KL_OVRP_IPD", 0.0f);
        forced = (v > 0.0f && v < 0.2f) ? v : 0.0f;
    }
    if (forced > 0.0f) {
        *ox = (eye == 1 ? 0.5f : -0.5f) * forced; *oy = 0.0f; *oz = 0.0f;
        return;
    }
    *ox = g_eye_off[eye][0]; *oy = g_eye_off[eye][1]; *oz = g_eye_off[eye][2];
}

// ovrp_GetUserIPD2(float* ipd) — real 0x170e60, -1001 on NULL, writes s0 and
// returns 0. 1.40's libOculusXRPlugin asks for it; 1.28 never did (kl_ovrp.h's
// note that "there is no ovrp_GetUserIPD in the surface this title imports" was
// true of that version and is not of this one).
//
// Derived from the SAME eye offsets everything else answers from, rather than
// carried as a second number: the IPD is the distance between the two eye
// positions, so answering it independently is how a guest gets a stereo
// separation from one entry point that disagrees with the one it renders with.
// It therefore also honours KL_OVRP_IPD for free.
static uint64_t klovrp_GetUserIPD2(float *out) {
    ovrp_hit("ovrp_GetUserIPD2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    float lx, ly, lz, rx, ry, rz;
    klovrp_eye_offset(0, &lx, &ly, &lz);
    klovrp_eye_offset(1, &rx, &ry, &rz);
    float dx = rx - lx, dy = ry - ly, dz = rz - lz;
    *out = sqrtf(dx * dx + dy * dy + dz * dz);
    return OVRP_SUCCESS;
}

// ...and the un-suffixed form, which is trap 10b's pair shape once more:
// `float ovrp_GetUserIPD(void)`, the value in s0 and no arguments at all.
//
// The real 0x1309c0 is a WRAPPER around the 2-form — it stack-allocates a float,
// pre-zeroes it, calls ovrp_GetUserIPD2, and `fcsel`s between 0.0f and the
// written slot on `w0 < 0`. So it is not an independent measurement and must not
// be implemented as one; answering it from anywhere but klovrp_GetUserIPD2 is a
// stereo separation that can disagree with the one the eyes are rendered with.
//
// BONELAB is the first guest to call it: it comes from managed code (the C#
// `OVRP_1_3_0.ovrp_GetUserIPD` binding), where the return convention is the
// whole contract — return OVRP_SUCCESS's 0 here by mistake and Unity gets an IPD
// of 0.0 rather than an error, which is SL-12's finding one API over: zero is
// read as "the eyes coincide", not as "nobody answered".
static float klovrp_GetUserIPD(void) {
    ovrp_hit("ovrp_GetUserIPD");
    float ipd = 0.0f;
    if ((int64_t)klovrp_GetUserIPD2(&ipd) < 0) return 0.0f;
    return ipd;
}

// Fills four f32 fov tangents at out+0x08..+0x14 (0x9bcbd4). libunity divides
// by their max for the aspect, so 0 is not survivable.
//
// The order here is ovrpFovf — UpTan, DownTan, LeftTan, RightTan — which is not
// the order the seam speaks, and this is the one place that transposition
// belongs: everything above the ABI uses Compositor Services' (left, right,
// top, bottom) so the compositor never has to remember two conventions. With
// the default symmetric frustum the two are indistinguishable, which is exactly
// why getting it wrong here would stay invisible until the day a real
// asymmetric field of view is pushed in.
//
// So the order is read, not assumed — out of the guest's own metadata, the same
// way the node ids and controller masks were. global-metadata.dat's string
// table has `zNear, zFar` adjacent (ovrpFrustum2f) and `UpTan, DownTan,
// LeftTan, RightTan` adjacent (ovrpFovf), in declaration order, which pins both
// the struct layout and this transposition.
static uint64_t klovrp_GetNodeFrustum2(int node, void *out) {
    ovrp_hit("ovrp_GetNodeFrustum2");
    // WHICH node the guest asks about, once per (node, caller).
    //
    // The fallback below is only "true enough to be honest" while the display is
    // near-symmetric, and Vision Pro's is not: its per-eye tangents are mirror
    // images (l=1.732/r=1.000 against l=1.000/r=1.732), so the optical axis sits
    // at 63.4% of the width in one eye and 36.6% in the other. A guest that asks
    // about EyeCenter — or about anything that is not exactly node 1 — is
    // therefore handed the LEFT eye's cone for both eyes, renders one picture,
    // and the composite then places it on two quads built from two different
    // asymmetric frusta. Measured on BONELAB: the two eye images differ by 0.4%
    // of the width where honouring the frusta would make it 26.8%.
    //
    // None of that has an error surface — every call returns success — so the
    // node number is the one thing worth naming.
    ovrp_log_arg("ovrp_GetNodeFrustum2", node, __builtin_return_address(0));
    // Nodes 0/1 are EyeLeft/EyeRight; anything else asking about a frustum
    // (EyeCenter, Head) gets the left eye's, which is what a symmetric or
    // near-symmetric display makes true enough to be honest.
    const float *t = klovrp_eye_tan(node == 1 ? 1 : 0);
    float *f = (float *)out;
    f[2] = t[2];    // UpTan    <- top
    f[3] = t[3];    // DownTan  <- bottom
    f[4] = t[0];    // LeftTan  <- left
    f[5] = t[1];    // RightTan <- right
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetTiledMultiResLevel(int* out) {
    if (out) *out = 0; // TiledMultiResLevel.Off
    return OVRP_TRUE;                       // ovrpBool
}

// ovrpTrackingOrigin: EyeLevel=0, FloorLevel=1, Stage=2. This decides what
// space every pose we report is *in*, so it cannot stay a discarded argument
// on the generic yes-stub: under FloorLevel/Stage the guest expects the head
// to sit at standing eye height above y=0, and an identity (y=0) head puts the
// camera on the floor — the menu then renders above the view and the hands are
// below the floor, out of frustum. Recorded here and read by the pose defaults.
static int g_tracking_origin;          // 0 = eye level, until the guest says otherwise
int kl_ovrp_tracking_origin(void) { return g_tracking_origin; }
//
// **The v1 and v2 spellings of this pair are four different ABIs, not two**, and
// three of the four were wrong here. Read out of the real plugin (SUPERHOT's
// +0x270f4 and beatsaber's +0x176390 agree):
//
//   ovrp_SetTrackingOriginType (origin)      -> ovrpBool   (1 on success)
//   ovrp_SetTrackingOriginType2(origin)      -> ovrpResult (0 on success)
//   ovrp_GetTrackingOriginType()             -> the ORIGIN, returned in w0
//   ovrp_GetTrackingOriginType2(int *out)    -> ovrpResult, origin through x0
//
// The getter's v1 form takes NO ARGUMENT: the real one calls the 2-form with a
// stack slot and answers `csel w0, wzr, w8, lt`, i.e. the value it read, or 0 if
// the call failed. Ours took an `int *` and STORED through it, so a guest
// calling the argument-less form had whatever happened to be in x0 written to —
// a wild store from a getter, which is a SIGBUS or silent corruption depending
// on what the register held, and unattributable to this function from anywhere.
// It survived because Beat Saber reaches the origin through the 2-form.
// Trap 10b's shape with the halves swapped: there the 2-form was implemented as
// a scalar return, here the scalar form was implemented as an out-param.
static uint64_t klovrp_SetTrackingOriginType(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType");
    ovrp_log_arg("ovrp_SetTrackingOriginType", origin, __builtin_return_address(0));
    g_tracking_origin = origin;
    return OVRP_TRUE;                       // ovrpBool
}
static uint64_t klovrp_SetTrackingOriginType2(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType2");
    ovrp_log_arg("ovrp_SetTrackingOriginType2", origin, __builtin_return_address(0));
    g_tracking_origin = origin;
    return OVRP_SUCCESS;                    // ovrpResult
}
// The plugin's own event queue, which OVRManager drains every frame.
//
// Signature read straight out of the real 1.59 body (+0x266dc), because both
// arguments are pointers it WRITES and getting that wrong is trap 10b:
//
//   ovrpResult ovrp_PollEvent2(ovrpEventType *type, void **eventData)
//
// It refuses either NULL with -1001, answers -1002 with no plugin, and when its
// internal fetch comes back empty it answers **-1006** without touching either
// pointer. That last one is the steady state here: we generate no plugin events
// at all — no display refresh changes, no boundary or tracking-state
// transitions, none of the things the queue carries — so every poll is an empty
// one. Answering ovrpSuccess instead would tell the guest an event IS waiting
// and hand it an uninitialised type and pointer to parse.
//
// Deliberately NOT a shared-handler entry: the out-params are the whole call.
static uint64_t klovrp_PollEvent2(uint32_t *type, void **data) {
    ovrp_hit("ovrp_PollEvent2");
    if (!type || !data) return OVRP_FAIL_INVALID_PARAM;
    return OVRP_FAIL_OPERATION;                    // the queue is empty
}

// The v1 spelling takes the buffer by reference and fills a type field inside
// it; same empty answer, and the same refusal for a NULL.
static uint64_t klovrp_PollEvent(void *buf) {
    ovrp_hit("ovrp_PollEvent");
    if (!buf) return OVRP_FAIL_INVALID_PARAM;
    return OVRP_FAIL_OPERATION;
}

static uint64_t klovrp_GetTrackingOriginType(void) {
    ovrp_hit("ovrp_GetTrackingOriginType");
    return (uint64_t)(uint32_t)g_tracking_origin;
}
static uint64_t klovrp_GetTrackingOriginType2(int *out) {
    ovrp_hit("ovrp_GetTrackingOriginType2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_tracking_origin;
    return OVRP_SUCCESS;
}

// The per-frame node loop's tracked/valid probes (0x9bbe78..0x9bbeb4): u32
// out-param, which is why these cannot be shared handlers. Tracked and valid
// is the honest answer for the head we pose ourselves.
//
// These return ovrpResult, NOT ovrpBool — the answer is the out-param and
// SUCCESS IS 0. Returning OVRP_TRUE (1) here read as a failure code, and the
// two callers disagree about how much that matters: libunity's node loop only
// reads the out-param, so its nodes tracked fine and this stayed invisible,
// while managed OVRPlugin checks `result == Result.Success` first and answers
// false without ever looking at the out-param. That is what made
// OVRInput.GetControllerPositionValid false with the controllers connected and
// tracked — and with it Beat Saber's controller poses, its laser and every UI
// hit, since VRController takes a pose only from a valid node.
static uint64_t node_tracked(const char *name, int n, uint32_t *out) {
    ovrp_hit(name);
    ovrp_log_arg(name, n, __builtin_return_address(0));
    *out = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetNodePositionTracked2(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodePositionTracked2", n, o);
}
static uint64_t klovrp_GetNodePositionValid(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodePositionValid", n, o);
}
static uint64_t klovrp_GetNodeOrientationValid(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodeOrientationValid", n, o);
}

// The head pose the frontend last gave us (kl_ovrp_set_head_pose — the seam
// declared in kl_ovrp.h). Identity by default, so a headless run sees exactly
// what it always saw.
// Motion rides in the same struct as the pose, and that is the point: every
// latch in this file (the frontend seqlock, the per-frame pin, the per-step
// sample) copies a klovrp_pose by value, so velocity that lives here is
// automatically coherent with the orientation it belongs to. A parallel array
// would have to repeat all three latches and would eventually disagree with
// one of them. Zero for the head and the eyes, which have no velocity source.
typedef struct {
    float px, py, pz, qx, qy, qz, qw;
    float vx, vy, vz;        // linear velocity, m/s, tracking space
    float avx, avy, avz;     // angular velocity, rad/s, tracking-space axes
    // ...and whether those six mean anything. A frontend that publishes a pose
    // and no motion used to leave them zero, which is not "unknown" — it is the
    // assertion that the thing is STATIONARY, made about a controller whose
    // position is visibly changing. OpenXR has a field for exactly this
    // distinction (XrSpaceVelocity.velocityFlags) and kl_openxr was careful to
    // use it; the zeros defeated that care from the publishing side.
    int   motion_valid;
} klovrp_pose;
static klovrp_pose g_head_pose = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0,   // motion_valid: nothing has been published, so there is nothing to know
};
static int g_head_set;              // has a frontend ever written a head pose?
// The two hands, published by the same frontend in the same breath. Declared
// here beside the head because the per-frame latch below promotes all three
// together — they are one sample of one instant and must stay so.
static klovrp_pose g_hand_pose[2];

// --- The frontend/guest pose handoff, and why it is a seqlock now -----------
//
// These used to be plain unsynchronised stores, on the argument that a torn
// read costs one frame of staleness and the frontend rewrites it next frame
// anyway. That argument was wrong in a way that did not matter yet, and
// PLANNING §12.12 is what makes it matter:
//
//  - A torn read is not a *stale* pose, it is an *incoherent* one — half of one
//    frame's rotation with half of another's, a pose that never existed.
//  - Until the guest was decoupled from the compositor thread, that could
//    barely happen: on device the writer and the reader were the same thread,
//    so the reader could only see a fully-written value because it *was* the
//    writer. That is no longer true of either frontend.
//  - And the consequence grew. Reprojection *subtracts* the pose a frame was
//    rendered with from the pose it is displayed at (kl_reproject.c), so an
//    incoherent latch is a wrong delta, and a wrong delta is a visible jump
//    rather than a shrug.
//
// A seqlock rather than a mutex, because the readers are on the guest's hot
// path — every ovrp_GetNodePoseState — while the writer is one thread at
// display rate. The retry count is **bounded**: past it the read is taken
// unsynchronised, which is exactly what this code did before, so a descheduled
// writer degrades to the old behaviour instead of spinning inside a frame.
static uint32_t g_pose_seq;         // even = stable, odd = a write in flight

static klovrp_pose klovrp_pose_read(const klovrp_pose *src) {
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(&g_pose_seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        klovrp_pose v = *src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pose_seq, __ATOMIC_RELAXED) == s) return v;
    }
    return *src;
}

// One sequence counter for the whole handoff, which is sound because there is
// exactly one writer: the frontend samples every pose for a frame from a single
// thread (the compositor's render loop, the viewer's UI thread) and pushes them
// through here. A second writer thread would need a real lock, so if one ever
// appears, this is the comment it invalidates.
// --- Deriving motion a frontend did not measure ------------------------------
//
// Two of the three publishers here report real velocity (KleptonControllers
// reads it off the Sense controllers); the macOS viewer and the parked default
// poses do not. Rather than tell the guest "unknown" for those, we DIFFERENTIATE
// the poses we are given — a real measurement of the same motion, one sample
// late, and better than either alternative: unknown makes a guest fall back to
// estimating anyway, and zero actively contradicts the positions we hand it in
// the same breath.
//
// The history is separate from the published pose because a pose-only publisher
// and a motion publisher must share one notion of "the previous sample" — if
// they did not, switching between them (the viewer's parked hands becoming live
// ones) would derive a velocity across a gap that is not motion.
//
// Single-writer, like the seqlock below and for the same reason: one frontend
// thread per process publishes these. A second writer needs a real lock, and
// this is the second comment it invalidates.
typedef struct {
    int    have;
    double t;
    float  px, py, pz, qx, qy, qz, qw;
} klovrp_motion_hist;
static klovrp_motion_hist g_head_hist, g_hand_hist[2];

static double klovrp_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void klovrp_hist_note(klovrp_motion_hist *h, const klovrp_pose *v) {
    h->have = 1;   h->t  = klovrp_mono_now();
    h->px = v->px; h->py = v->py; h->pz = v->pz;
    h->qx = v->qx; h->qy = v->qy; h->qz = v->qz; h->qw = v->qw;
}

// Fill v's velocity from the difference against `h`, then advance `h`.
static void klovrp_derive_motion(klovrp_pose *v, klovrp_motion_hist *h) {
    static int derive = -1;
    if (derive < 0) derive = kl_env_on("KL_OVRP_DERIVE_VELOCITY", 1);

    double now = klovrp_mono_now();
    double dt  = h->have ? now - h->t : 0.0;

    v->vx = v->vy = v->vz = v->avx = v->avy = v->avz = 0;
    v->motion_valid = 0;

    // A plausible frame's worth of time, and outside it the difference is not a
    // velocity: below the floor it is float noise divided by nearly zero, and
    // above the ceiling the two samples are not consecutive motion at all (a
    // paused guest, a first sample, a frontend that stopped publishing). Both
    // cases report UNKNOWN rather than a number, which is the whole point.
    if (derive && h->have && dt >= 1e-4 && dt <= 0.5) {
        float idt = (float)(1.0 / dt);
        v->vx = (v->px - h->px) * idt;
        v->vy = (v->py - h->py) * idt;
        v->vz = (v->pz - h->pz) * idt;

        // Angular velocity from the rotation BETWEEN the samples: q_d =
        // q_now * conj(q_prev), then axis-angle over dt. Negating a q_d with
        // w < 0 takes the short way round — quaternions double-cover, so
        // without it a small rotation can read as a nearly-2pi one.
        float cx = -h->qx, cy = -h->qy, cz = -h->qz, cw = h->qw;
        float dw = v->qw*cw - v->qx*cx - v->qy*cy - v->qz*cz;
        float dx = v->qw*cx + v->qx*cw + v->qy*cz - v->qz*cy;
        float dy = v->qw*cy - v->qx*cz + v->qy*cw + v->qz*cx;
        float dz = v->qw*cz + v->qx*cy - v->qy*cx + v->qz*cw;
        if (dw < 0) { dw = -dw; dx = -dx; dy = -dy; dz = -dz; }
        float s = sqrtf(dx*dx + dy*dy + dz*dz);
        if (s > 1e-8f) {
            // atan2 rather than acos: acos loses all its precision exactly
            // where this spends its time, at the small angles of one frame.
            float k = 2.0f * atan2f(s, dw) * idt / s;
            v->avx = dx * k; v->avy = dy * k; v->avz = dz * k;
        }
        v->motion_valid = 1;
    }
    klovrp_hist_note(h, v);
}

static void klovrp_pose_write(klovrp_pose *dst, const klovrp_pose *v) {
    uint32_t s = __atomic_load_n(&g_pose_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pose_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *dst = *v;
    __atomic_store_n(&g_pose_seq, s + 2, __ATOMIC_RELEASE);
}

// Standing eye height, and why the default head cannot be the origin.
// The guest sets ovrp_SetTrackingOriginType(FloorLevel) — measured, see that
// function — so y=0 is the *floor*, not the eye. An identity head under
// FloorLevel is a camera lying on the ground: Beat Saber's menu is authored at
// eye height, so it renders above the view, and anything parked near y=0 (the
// hands) is a metre and a half below the frustum. Under EyeLevel the eye *is*
// the origin, so the offset is zero and nothing moves.
static float klovrp_eye_height(void) {
    static int init;
    static float h = 1.6f;
    if (!init) {
        init = 1;
        h = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    }
    return g_tracking_origin == 0 ? 0.0f : h;
}

float kl_ovrp_eye_height(void) { return klovrp_eye_height(); }

// --- One pose per guest frame, and why the live one is wrong ---------------
//
// **This is what was left of the judder after the swapchain was fixed.**
//
// The frontend publishes a new pose every *display* frame, on its own thread
// (PLANNING §12.12). The guest reads poses through ovrp_GetNodePoseState
// whenever it likes during its own frame, and its frame is longer than a
// display frame whenever performance is short. So within one guest frame the
// answer to "where is the head" could change several times — and, worse, the
// pose recorded for timewarp (klovrp_BeginFrame, which latched the live value)
// was not necessarily any of the answers the guest was given.
//
// Reprojection subtracts the recorded pose from the display pose. If the guest
// rendered from P(T1) and we recorded P(T2), the correction is wrong by exactly
// P(T2) - P(T1): one guest frame of head rotation, applied backwards. A frame
// that is over-corrected followed by one that is under-corrected is not blur,
// it is **two images in two places** — the doubling seen on device during head
// rotation, growing as the frame rate falls, which is precisely when T2 - T1
// grows.
//
// So the guest gets ONE pose for the whole of its frame, promoted at its frame
// boundary, and that is the pose recorded. The record is then truthful by
// construction rather than by the two threads happening to be in step. This is
// what a real runtime does — a frame's poses come from one predicted instant —
// and it is the property kl_reproject.h's whole argument assumes.
//
// The hands come along for the ride: they are published in the same breath from
// the same ARKit query, and a frame that draws the head from one instant and
// the hands from another is incoherent in the same way, just less visibly.
// Controller *buttons* stay live — an edge is not a pose, and freshness there
// costs nothing.
//
// KL_OVRP_LATCH=0 restores the live read, which is every measurement taken
// before this and the A/B if the pinning is ever suspected of costing latency.
static klovrp_pose g_frame_head, g_frame_hand[2];
static uint32_t    g_frame_pose_seq;
static int         g_frame_latched;

static int klovrp_latch_enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_LATCH", 0);
    }
    return on;
}

// A seqlock of its own, because the writer is the *guest's* frame driver while
// g_pose_seq's writer is the frontend. Sharing one counter between two writer
// threads is exactly the bug that counter's comment warns about.
static klovrp_pose klovrp_seq_read(const klovrp_pose *src, const uint32_t *seq) {
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        klovrp_pose v = *src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(seq, __ATOMIC_RELAXED) == s) return v;
    }
    return *src;
}

// The head pose as PUBLISHED — where the frontend says the head is now. This is
// the display side's question, not the guest's: the compositor asks it to
// reproject towards, and the viewer asks it to drive its own composite.
static klovrp_pose klovrp_head_published(void) {
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    return h;
}

// The head pose as the GUEST sees it: pinned for the whole of its frame.
static klovrp_pose klovrp_head(void) {
    if (klovrp_latch_enabled() && __atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        klovrp_pose h = klovrp_seq_read(&g_frame_head, &g_frame_pose_seq);
        if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
        return h;
    }
    return klovrp_head_published();
}

// The shortest angle between two orientations, in degrees. Used only to report
// how much motion the latch is absorbing.
static float klovrp_quat_degrees(const klovrp_pose *a, const klovrp_pose *b) {
    float d = a->qx * b->qx + a->qy * b->qy + a->qz * b->qz + a->qw * b->qw;
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * (180.0f / 3.14159265358979f);
}

// Promote the published poses to the ones this guest frame will see. Called at
// the top of the guest's frame, before anything in it can ask.
//
// The number it reports is the measurement that justifies the whole mechanism:
// how far the head moved during the *previous* guest frame, i.e. how wrong the
// recorded pose used to be. At a comfortable frame rate it is a fraction of a
// degree; when the guest is struggling it is whole degrees, and a whole degree
// of mis-correction is plainly visible.
void kl_ovrp_frame_latch(void) {
    if (!klovrp_latch_enabled()) return;
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    klovrp_pose l = klovrp_pose_read(&g_hand_pose[0]);
    klovrp_pose r = klovrp_pose_read(&g_hand_pose[1]);

    static float worst;
    static unsigned n;
    if (__atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        float moved = klovrp_quat_degrees(&g_frame_head, &h);
        if (moved > worst) worst = moved;
    }
    if (++n % 300 == 0) {
        fprintf(stderr, "  [ovrp] pose latch: worst %.2f deg of head motion "
                        "inside one guest frame over the last 300\n", (double)worst);
        worst = 0;
    }

    uint32_t s = __atomic_load_n(&g_frame_pose_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_frame_pose_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_frame_head = h; g_frame_hand[0] = l; g_frame_hand[1] = r;
    __atomic_store_n(&g_frame_pose_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_frame_latched, 1, __ATOMIC_RELEASE);
}

// The hand pose as the guest sees it — pinned with the head, for the same
// reason and at the same instant.
static klovrp_pose klovrp_hand(int hand) {
    if (klovrp_latch_enabled() && __atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_frame_hand[hand], &g_frame_pose_seq);
    return klovrp_pose_read(&g_hand_pose[hand]);
}

// v' = q ⊗ v ⊗ q⁻¹ for a unit quaternion, expanded — used to carry a
// head-relative offset into tracking space.
static void klovrp_qrot(const klovrp_pose *q, float vx, float vy, float vz,
                        float *ox, float *oy, float *oz) {
    float tx = 2.0f * (q->qy * vz - q->qz * vy);
    float ty = 2.0f * (q->qz * vx - q->qx * vz);
    float tz = 2.0f * (q->qx * vy - q->qy * vx);
    *ox = vx + q->qw * tx + (q->qy * tz - q->qz * ty);
    *oy = vy + q->qw * ty + (q->qz * tx - q->qx * tz);
    *oz = vz + q->qw * tz + (q->qx * ty - q->qy * tx);
}

// The Hamilton product, a then b applied as "b first, then a" — so
// world_from_eye = world_from_device ⊗ device_from_view.
static klovrp_pose klovrp_qmul(const klovrp_pose *a, const float b[4]) {
    klovrp_pose r = *a;
    float ax = a->qx, ay = a->qy, az = a->qz, aw = a->qw;
    float bx = b[0],  by = b[1],  bz = b[2],  bw = b[3];
    r.qw = aw * bw - ax * bx - ay * by - az * bz;
    r.qx = aw * bx + ax * bw + ay * bz - az * by;
    r.qy = aw * by - ax * bz + ay * bw + az * bx;
    r.qz = aw * bz + ax * by - ay * bx + az * bw;
    return r;
}

// The eye view, for a guest that does not speak OVRPlugin — see kl_ovrp.h.
// This is klovrp_GetNodePoseState's node-0/1 arm and klovrp_GetNodeFrustum2's
// tangents, without the ABI: same latched head, same offsets, same cant, same
// frustum. Deliberately NOT a reimplementation — the whole reason it lives in
// this file is that the pieces it composes are the ones that already carry the
// per-frame latch and the display's measured geometry.
void kl_ovrp_eye_view(int eye, float *px, float *py, float *pz,
                      float *qx, float *qy, float *qz, float *qw,
                      float tangents[4]) {
    if ((unsigned)eye > 1) eye = 0;
    klovrp_pose head = klovrp_head();

    float dx, dy, dz, ox, oy, oz;
    klovrp_eye_offset(eye, &dx, &dy, &dz);
    klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
    klovrp_pose e = klovrp_eye_cant() ? klovrp_qmul(&head, g_eye_rot[eye]) : head;

    if (px) *px = head.px + ox;
    if (py) *py = head.py + oy;
    if (pz) *pz = head.pz + oz;
    if (qx) *qx = e.qx; if (qy) *qy = e.qy;
    if (qz) *qz = e.qz; if (qw) *qw = e.qw;
    if (tangents) memcpy(tangents, klovrp_eye_tan(eye), sizeof g_eye_tan[eye]);
}

// The guest's head — the latched one. See the header for why this is a
// different function from kl_ovrp_get_head_pose rather than a parameter on it.
void kl_ovrp_get_guest_head_pose(float *px, float *py, float *pz,
                                 float *qx, float *qy, float *qz, float *qw) {
    klovrp_pose h = klovrp_head();
    if (px) *px = h.px; if (py) *py = h.py; if (pz) *pz = h.pz;
    if (qx) *qx = h.qx; if (qy) *qy = h.qy;
    if (qz) *qz = h.qz; if (qw) *qw = h.qw;
}

void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    // DeviceAnchor does not report a velocity, so this derives one. The old
    // note here said the head's motion "is not a field the guest reads for
    // node 9 anyway" — true of an OVRPlugin guest, and node 9 is still served
    // from the same sample. It stopped being the whole story when an OpenXR
    // guest arrived: velocity there is a chained output struct on any space,
    // and zeros in it are an assertion rather than a silence.
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0 };
    klovrp_derive_motion(&v, &g_head_hist);
    klovrp_pose_write(&g_head_pose, &v);
    __atomic_store_n(&g_head_set, 1, __ATOMIC_RELEASE);
}

// The frontend's question — "where is the head NOW" — so it reads the published
// pose, not the one pinned for the guest's frame. The viewer's composite uses
// this as the pose to reproject *towards*, and reprojecting towards the pose
// the picture was already drawn with would be a no-op.
void kl_ovrp_get_head_pose(float *px, float *py, float *pz,
                           float *qx, float *qy, float *qz, float *qw) {
    klovrp_pose h = klovrp_head_published();
    if (px) *px = h.px; if (py) *py = h.py; if (pz) *pz = h.pz;
    if (qx) *qx = h.qx; if (qy) *qy = h.qy;
    if (qz) *qz = h.qz; if (qw) *qw = h.qw;
}

// --- ovrp_Update2: the guest's own latch point ---------------------------
//
// **This is the concrete signal we had been guessing around.** OVRPlugin's
// contract is that tracking is sampled once per frame per *step* — the guest
// calls ovrp_Update2(step, frameIndex, predictionSeconds), and every
// ovrp_GetNodePoseState(step, node) afterwards returns that sample. Measured on
// this title: 85 Update2 calls across 38 frames (two steps a frame) and 760
// GetNodePoseState calls, i.e. twenty reads per frame off two samples.
//
// We were answering Update2 from the shared ignore-the-arguments handler and
// serving every read from a live global that the frontend rewrites at display
// rate. So the twenty reads inside one guest frame could return twenty
// different poses, the Render and Physics steps collapsed into one drifting
// value, and the pose recorded for timewarp was not necessarily any of the
// answers the guest was actually given. Reprojection subtracts that record —
// so the correction was against a pose that never rendered anything.
//
// Now the guest's own call is the boundary. Nothing here is inferred: the step,
// the frame index and the moment all come from the guest.
//
//   ovrpStep_Render = -1, ovrpStep_Physics = 0   (OVRPlugin.Step)
#define KLOVRP_STEP_RENDER (-1)
#define KLOVRP_NSTEPS 2
static inline int klovrp_step_ix(int step) { return step == KLOVRP_STEP_RENDER ? 0 : 1; }

static klovrp_pose g_step_head[KLOVRP_NSTEPS];
static klovrp_pose g_step_hand[KLOVRP_NSTEPS][2];
static uint32_t    g_step_seq;
static int         g_step_valid[KLOVRP_NSTEPS];
static int         g_saw_update2;

// The render step's sample, kept apart so BeginFrame can record exactly what
// the guest was told to render with rather than re-reading anything.
static klovrp_pose g_render_sample;

static uint64_t klovrp_Update2(int step, int frame_index, double prediction) {
    ovrp_hit("ovrp_Update2");
    (void)frame_index; (void)prediction;
    int ix = klovrp_step_ix(step);

    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    klovrp_pose l = klovrp_pose_read(&g_hand_pose[0]);
    klovrp_pose r = klovrp_pose_read(&g_hand_pose[1]);

    // How much the head moved between this frame's sample and the last one for
    // the same step. This is the quantity the old code was silently absorbing
    // into the timewarp delta, so it is worth being able to read.
    static float worst;
    static unsigned n;
    if (ix == 0 && g_step_valid[0]) {
        float moved = klovrp_quat_degrees(&g_step_head[0], &h);
        if (moved > worst) worst = moved;
        if (++n % 300 == 0) {
            fprintf(stderr, "  [ovrp] Update2(Render): worst %.2f deg between "
                            "consecutive samples over the last 300\n", (double)worst);
            worst = 0;
        }
    }

    uint32_t s = __atomic_load_n(&g_step_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_step_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_step_head[ix] = h;
    g_step_hand[ix][0] = l; g_step_hand[ix][1] = r;
    if (ix == 0) g_render_sample = h;
    __atomic_store_n(&g_step_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_step_valid[ix], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_saw_update2, 1, __ATOMIC_RELEASE);

    // Return value deliberately UNCHANGED from the shared handler this replaces.
    // OVRPlugin declares ovrp_Update2 returning Bool, where 0 is false, and both
    // libunity and libil2cpp reference the name — which is trap 10's exact
    // shape. It may well want OVRP_TRUE. But it has answered 0 for every
    // measurement taken so far, and changing the latch and the return in one
    // step would make the next device run unreadable. One variable at a time.
    return OVRP_SUCCESS;
}

// ovrp_Update3(step, frameIndex, predictionSeconds) — same three arguments and
// the same latch, one ABI revision on (real 0x16eb90, and unlike ...2 it is
// unambiguously ovrpResult: every exit is `mov w0, wzr` or a negative code).
// 1.40's libOculusXRPlugin resolves both and calls whichever it has.
static uint64_t klovrp_Update3(int step, int frame_index, double prediction) {
    uint64_t r = klovrp_Update2(step, frame_index, prediction);
    // klovrp_Update2 records itself as ovrp_Update2; correct the attribution so
    // the work list says which numbered form the guest actually called.
    int s2 = ovrp_slot("ovrp_Update2");
    if (s2 >= 0 && g_ovrp[s2].calls) g_ovrp[s2].calls--;
    ovrp_hit("ovrp_Update3");
    return r;
}

// The pose for a given step, as the guest was told it. Falls back to the live
// value only where Update2 has never been seen — a headless run, or a guest
// that does not use this part of the API.
static klovrp_pose klovrp_step_head(int step) {
    int ix = klovrp_step_ix(step);
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_step_head[ix], &g_step_seq);
    return klovrp_head();
}

static klovrp_pose klovrp_step_hand(int step, int hand) {
    int ix = klovrp_step_ix(step);
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_step_hand[ix][hand], &g_step_seq);
    return klovrp_hand(hand);
}

// --- Timewarp bookkeeping ---------------------------------------------------
// See kl_ovrp.h for what this is for and why it is keyed to the stage. The
// consumer is kl_reproject.c / the two compositors.
//
// Locked, unlike the pose above. The pose is written and read every frame by
// two threads and a torn read costs one frame of staleness; a *record* is
// different — half of one frame's rotation with half of another's is a pose
// that never existed, and reprojecting against it would produce a visible jump
// rather than a shrug. The lock is taken twice per frame per side and is
// uncontended in practice.
#define KLOVRP_MAX_STAGES 4
static struct {
    kl_ovrp_render_pose r[KLOVRP_MAX_STAGES];
    // The frame between BeginFrame and EndFrame, whose stage is not known yet.
    kl_ovrp_render_pose pending;
    int                 pending_index;   // the GUEST's frame index
    uint64_t            stage_disagree;  // index%%N vs the observed stage
    // How the observation window closed, counted per frame. These are the
    // numbers that say whether the pose↔picture association is *known* or
    // merely available — see klovrp_EndFrame.
    uint64_t            unobserved;      // the frame drew into no eye stage
    uint64_t            multi;           // ...into more than one
    uint64_t            cross_thread;    // drawn by a thread that is not this one
    // Frames filed per stage, and how many took the counter fallback. The one
    // number that says whether the swapchain is really cycling: a second stage
    // that never receives a frame is memory spent to fix nothing, and the
    // read-while-writing race it exists to remove is still there.
    uint64_t            filed[KLOVRP_MAX_STAGES];
    uint64_t            guessed;
    // Frames filed on the stage the GUEST named in its submit, and how many of
    // those the frame counter would have put somewhere else. The second number
    // is the whole justification for reading the submit at all: if it is 0 the
    // counter was right all along, and if it is large every compositor frame on
    // the Vulkan path was sampling the wrong stage before this existed.
    uint64_t            named;
    uint64_t            named_disagree;
    uint64_t            serial;         // frames begun
    int                 last_complete;  // stage of the last completed frame, -1 = none
    pthread_mutex_t     mu;
} g_frames = { .last_complete = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

// How many swapchain stages the guest is told it has. KL_OVRP_STAGES is the
// A/B in both directions.
//
// One stage single-buffers the eye textures. The fence runs in one direction
// only (kl_glfb signals at eglSwapBuffers, the composite waits on it), so "the
// guest finished frame N" is ordered but "the composite finished reading frame
// N" is not: frame N+1 overwrites the texture while the composite still has it
// bound, across two Metal queues that order nothing between them. Standing
// still the two pictures are nearly identical; turning the head they differ and
// the composite samples a mixture. Worse at higher resolution, where a longer
// guest frame overlaps the composite for longer.
//
// 3 rather than 2 because the guest is decoupled from the compositor (PLANNING
// §12.12): one spare buffer only just covers a guest running a frame ahead,
// leaving nothing for a frame that runs long.
//
// The stage a frame drew into is MEASURED, not derived from a frame counter —
// klovrp_EndFrame files each record under the stage kl_glfb watched the guest
// draw into, windowed by BeginFrame/EndFrame so a frame that drew into no stage
// is reported as such rather than inheriting the previous frame's answer (the
// off-by-one that pairs a fresh pose with a stale picture). Host, 300 frames at
// two and three stages: every frame drew into exactly one stage, on EndFrame's
// thread, and the guest's frame index % stages agreed on 298 of them.
//
// Cost is memory: an eye texture is RGBA16F with two array slices, so at map
// resolution each stage is on the order of 160 MB, and a swapchain re-creation
// transiently holds two generations.
#define KLOVRP_STAGES_DEFAULT 3

int kl_ovrp_stage_count(void) {
    static int n;
    if (!n) {
        n = kl_env_int("KL_OVRP_STAGES", KLOVRP_STAGES_DEFAULT);
        if (n < 1) n = 1;
        if (n > KLOVRP_MAX_STAGES) n = KLOVRP_MAX_STAGES;
    }
    return n;
}

// libunity's frame dispatcher (0x9bb808) calls BeginFrame, EndEye2 twice, then
// EndFrame. This is where the pose the frame will be rendered with is fixed:
// the frontend wrote it before driving this frame, and every
// ovrp_GetNodePoseState the guest is about to make will answer the same thing.
//
// The argument is the guest's own frame index, and it is deliberately ignored:
// our own counter is what the ring is indexed by, so a guest that numbers
// frames differently — or restarts them — cannot desynchronise the ring from
// the records in it.
static uint64_t klovrp_begin_frame_impl(int guest_frame_index) {
    // The RENDER step's sample — the pose the guest was handed for this frame
    // and rendered every eye from. Not a fresh read: a fresh read at this
    // moment is a pose the picture was never drawn with, and reprojection
    // subtracts whatever is recorded here.
    klovrp_pose h = __atomic_load_n(&g_saw_update2, __ATOMIC_ACQUIRE)
                    ? klovrp_step_head(KLOVRP_STEP_RENDER) : klovrp_head();
    pthread_mutex_lock(&g_frames.mu);
    uint64_t s = ++g_frames.serial;
    // Into the PENDING record, not into a stage. Which stage this frame goes to
    // is the guest's choice and is not knowable yet — it is read off the draw
    // target at EndFrame. Filing it here under a guessed stage is what the old
    // one-stage code got away with and what more than one stage would break.
    kl_ovrp_render_pose *r = &g_frames.pending;
    r->px = h.px; r->py = h.py; r->pz = h.pz;
    r->qx = h.qx; r->qy = h.qy; r->qz = h.qz; r->qw = h.qw;
    // The frustum is recorded per frame rather than read live by the
    // compositor, because a frontend may push a new one at any time: a picture
    // rendered with the old field of view must keep being placed with the old
    // field of view, or it is resized by a change that happened after it.
    // Through klovrp_eye_tan, NOT g_eye_tan: this is what the composite
    // builds its quad from, so it has to be the frustum the guest was
    // actually told to render with, unified or not.
    for (int e = 0; e < 2; e++)
        memcpy(r->tangents[e], klovrp_eye_tan(e), sizeof r->tangents[e]);
    // Cleared, not carried: the render viewport is a per-frame statement the
    // guest makes at EndFrame4, and a rect left over from the previous frame
    // would keep cropping after the guest stopped asking for it. Zero reads as
    // "the whole texture" everywhere (kl_ovrp.h).
    memset(r->viewport, 0, sizeof r->viewport);
    memset(r->viewport_of, 0, sizeof r->viewport_of);
    r->serial = s;
    r->stage = -1;
    r->complete = 0;
    // Unity's own frame counter — the number it picks its stage from.
    g_frames.pending_index = guest_frame_index;
    pthread_mutex_unlock(&g_frames.mu);
    // Open the observation window. Everything the guest binds as a draw target
    // from here until EndFrame is what THIS frame committed to; a sticky
    // "last stage" without a window answers with the previous frame's when this
    // one drew nothing, and that answer is indistinguishable from a right one.
    kl_glfb_begin_render_window();
    return OVRP_SUCCESS;
}

// The two named entry points over the core above. They exist separately only
// because the work-list report counts by the name a call actually hit — a
// single shared pointer would label every ovrp_BeginFrame4 as the un-numbered
// form. The work is identical either way and neither carries an out-param.
static uint64_t klovrp_BeginFrame(int guest_frame_index) {
    ovrp_hit("ovrp_BeginFrame");
    return klovrp_begin_frame_impl(guest_frame_index);
}

// The 1.40 wrapper's frame-begin (real 0x16ec80). Same job, plus a second
// argument the real plugin only forwards to the runtime (x1 -> internal x3);
// nothing about the frame record depends on it, so it is named and dropped.
// On success the real one also pokes a global "a frame is in flight" byte;
// our record rings already express that, and answering ovrpSuccess is all any
// caller reads.
static uint64_t klovrp_BeginFrame4(int guest_frame_index, uint64_t extra) {
    ovrp_hit("ovrp_BeginFrame4");
    (void)extra;
    return klovrp_begin_frame_impl(guest_frame_index);
}

// The guest has finished submitting this frame's eyes. Only now is the stage
// safe for a compositor to sample; before it, the record describes a picture
// that is still being drawn — and only now is the stage even *known*.
//
// The stage comes from kl_glfb, which watched which eye texture the guest bound
// as its draw target. It is not derived from our own frame counter any more:
// the counter agrees with the guest's cycle only by luck, and disagreeing means
// pairing one frame's picture with another frame's pose, which reprojection
// then "corrects" by a delta that was never real. Where the observation is
// unavailable — the null GL driver, `make check`, any run without KL_GLFB —
// the counter is still the fallback, and with a single stage it is exact.
// The eye layer's ViewportRect pair out of an ovrpLayerSubmit array, as eight
// ints {x,y,w,h} per eye. Defined with the layer family below, where every
// other ABI this guest's display provider speaks is transcribed. Returns 0 and
// leaves `vp` alone when there is nothing to read.
static int klovrp_submit_viewports(const void *layer_submits, int count, int vp[8],
                                   int of[2]);
// Which texture stage the guest says it just drew the EYE layer into. Forward
// declared for the same reason as the line above: ovrpLayerSubmit's layout is
// stated further down, next to the DWARF it was read out of.
static int klovrp_submit_stage(const void *layer_submits, int count);
// ...and the census of the whole list, for the same forward-declaration reason.
static void klovrp_census_submits(const void *layer_submits, int count);
// ...and the overlay list a compositor draws on top of the eye picture.
static void klovrp_record_overlays(const void *layer_submits, int count);

// `viewports` is that pair, or NULL where a caller cannot know — ovrp_EndFrame
// (1.28's legacy VRDevice, which has no layers at all) and every non-Unity
// guest. NULL leaves the record's zeroes in place, i.e. "the whole texture".
// `viewport_of` is the texture size those rects are relative to, and travels
// with them for the reason kl_ovrp.h gives: separately they can drift, and the
// drift is a wrong picture with nothing reporting it.
//
// `named_stage` is the stage the guest itself said it drew into — the
// `TextureStage` field of the eye layer's submit — or -1 where there is no
// submit list to read it from. It sits between the GL observation and the frame
// counter in the ordering below, and it exists because on the VULKAN path the
// observation cannot work at all: kl_glfb watches GL framebuffer attachments and
// a Vulkan guest makes none, so every frame would fall through to
// `serial % stages`. That counter is measurably wrong on the XR-SDK display
// provider (8786 disagreements in a 9000-frame 1.40 run, see EndFrame4), and a
// compositor keyed on kl_ovrp_last_complete_stage() would then sample the stage
// the guest is drawing into NEXT — a torn picture with every counter healthy.
static uint64_t klovrp_end_frame_impl(int guest_frame_index, const int *viewports,
                                      const int *viewport_of, int named_stage) {
    // Close the observation window opened at BeginFrame. `observed` is still
    // the sticky answer; `mask`/`binds` are what say whether it belongs to this
    // frame, and they are the difference between an association that is known
    // and one that is merely plausible.
    uint32_t mask = 0, binds = 0;
    uint64_t draw_tid = 0;
    int observed = kl_glfb_render_stages(&mask, &binds, &draw_tid);
    int stages = kl_ovrp_stage_count();
    int nstages = __builtin_popcount(mask);

    // The three ways the window can close badly, each named the first time it
    // happens. All three are silent under a sticky global — that is the point.
    if (kl_glfb_has_mtl_provider()) {
        uint64_t self = 0;
        pthread_threadid_np(NULL, &self);
        static int said_none, said_multi, said_thread;
        if (binds == 0) {
            g_frames.unobserved++;
            // Not held under the lock: these are diagnostics on the guest's own
            // frame thread and a miscount is cheaper than a lock on this path.
            if (!said_none && observed >= 0) {
                said_none = 1;
                fprintf(stderr, "  [ovrp] frame %d drew into NO eye stage between "
                                "BeginFrame and EndFrame — it produced no new "
                                "picture, so its pose is DROPPED rather than filed "
                                "over a stage whose picture is older than it\n",
                        guest_frame_index);
            }
        } else if (nstages > 1) {
            g_frames.multi++;
            if (!said_multi) {
                said_multi = 1;
                fprintf(stderr, "  [ovrp] frame %d drew into %d stages (mask 0x%x) "
                                "— one pose per frame cannot describe that\n",
                        guest_frame_index, nstages, mask);
            }
        }
        if (binds && draw_tid && draw_tid != self) {
            g_frames.cross_thread++;
            if (!said_thread) {
                said_thread = 1;
                fprintf(stderr, "  [ovrp] eye draws are on thread %llu, EndFrame on "
                                "%llu — the window bounds nothing and the stage is "
                                "whatever that thread had reached\n",
                        (unsigned long long)draw_tid, (unsigned long long)self);
            }
        }
    }

    // Only worth saying when something is actually sampling by stage. Under the
    // null driver nothing renders and nothing composites, so an unobserved
    // stage is the expected state rather than a warning — and a warning that
    // fires on every diagnostic run is one nobody reads on the run that matters.
    if (observed < 0 && stages > 1 && kl_glfb_has_mtl_provider()) {
        // Loud, once. With one stage the counter is exact and this cannot
        // matter; with more than one it is a guess, and a wrong guess files
        // this frame's pose against the previous frame's picture — the same
        // mismatch multiple stages exist to remove. If this line appears, the
        // guest is attaching its eye textures through some entry point the
        // framebuffer thunks do not watch, and KL_OVRP_STAGES=1 is the way
        // back to a configuration that cannot be wrong.
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] eye stage NOT observed — falling back to "
                            "the frame counter with %d stages; if the picture "
                            "judders, try KL_OVRP_STAGES=1\n", stages);
        }
    }
    // **A frame that drew into no eye stage must not file anything.**
    //
    // Measured on device (PLANNING §12.19): it happens, and it never happens on
    // the host, which is why every host measurement came back clean. Such a
    // frame produced no new picture — so every stage still holds an image from
    // an *earlier* frame, and writing this frame's pose over any of them makes
    // the compositor reproject an old picture by a new pose. That is a delta
    // that was never real, applied to one stage out of N, i.e. one frame in N
    // displaced and the rest correct: temporal doubling whose period is the
    // stage count. Dropping the record instead leaves every stage describing
    // the picture it actually holds, and the compositor shows the previous
    // frame again — which is what a frame that drew nothing *should* look like.
    //
    // Only when the observation has never worked at all (`observed < 0` — the
    // null GL driver, `make check`, any run without KL_GLFB) does the counter
    // remain the fallback. There, nothing composites and nothing reads these
    // records, so the old behaviour is preserved rather than reasoned about.
    int drop = binds == 0 && observed >= 0;
    pthread_mutex_lock(&g_frames.mu);
    // Before the record is filed, and into the same record: the viewport
    // describes THIS frame's picture exactly as the pose and the tangents do,
    // and a compositor that reads one from the stage ring and the other from a
    // global would crop frame N's picture by frame N+1's rect on any run where
    // the guest changes it — which is the moment this exists for.
    if (viewports) memcpy(g_frames.pending.viewport, viewports, sizeof g_frames.pending.viewport);
    if (viewport_of) memcpy(g_frames.pending.viewport_of, viewport_of,
                            sizeof g_frames.pending.viewport_of);
    if (g_frames.serial && !drop) {
        // In order of how much the answer is *known*:
        //   the window saw exactly one stage      — measured, this frame's
        //   the window saw several                — measured but ambiguous, take
        //                                           the last and count it
        //   the guest NAMED one in its submit     — its own assertion, and the
        //                                           only answer available on a
        //                                           Vulkan guest
        //   nothing above                         — the counter, as before
        int stage;
        if (binds && nstages == 1)
            stage = __builtin_ctz(mask);
        else if (binds && observed >= 0 && observed < stages)
            stage = observed;
        else if (named_stage >= 0 && named_stage < stages) {
            stage = named_stage;
            g_frames.named++;
            if (named_stage != (int)((g_frames.serial - 1) % (unsigned)stages))
                g_frames.named_disagree++;
        } else
            stage = (int)((g_frames.serial - 1) % (unsigned)stages);
        g_frames.pending.stage = stage;
        g_frames.pending.complete = 1;
        g_frames.r[stage] = g_frames.pending;
        g_frames.last_complete = stage;
        g_frames.filed[stage]++;
        if (!binds) g_frames.guessed++;

        // **The measurement that decides how the stage should be derived.**
        // Unity picks the stage it renders into from its own frame counter, and
        // hands us that counter here and at BeginFrame. If `index % stages`
        // agrees with the FBO we watched it draw into, then the index is the
        // concrete answer and the sticky observation can go — and if they
        // disagree, the difference IS the off-by-one that makes two stages
        // double. Either way this stops being inferred.
        int from_index = ((guest_frame_index % stages) + stages) % stages;
        if (stages > 1 && observed >= 0) {
            static unsigned n;
            if (from_index != stage) g_frames.stage_disagree++;
            if (n++ < 8 || (from_index != stage && g_frames.stage_disagree < 4))
                fprintf(stderr, "  [ovrp] stage: guest frame %d %% %d = %d, "
                                "observed %d%s\n", guest_frame_index, stages,
                        from_index, observed,
                        from_index == stage ? "" : "   <-- DISAGREE");
        }
    }
    pthread_mutex_unlock(&g_frames.mu);

    // ...and this is the XR-SDK path's SWAP.
    //
    // kl_glfb's capture and both frontend seams hang off eglSwapBuffers, and
    // 1.40's display provider never calls it — measured `eglSwapBuffers: 0`
    // across a 58-frame run whose eye stages were demonstrably drawn into. So
    // KL_GLFB_OUT was silently inert on the one path that had pixels, which is
    // the same hole SL-13 found on the OpenXR path (kl_openxr.c's xrEndFrame
    // carries the identical block, for the identical reason).
    //
    // Gated on the guest never having swapped rather than on a version test:
    // 1.28's legacy VRDevice calls BOTH this and eglSwapBuffers, and presenting
    // twice a frame would double every capture and hand a frontend two frames
    // per frame. The swap count answers "is the swap this guest's presentation
    // signal?" directly, which is the actual question.
    if (kl_egl_swap_count() == 0) {
        const char *out = kl_env_str("KL_GLFB_OUT", NULL);
        if (kl_glfb_enabled() &&
            (out || kl_glfb_has_frame_sink() || kl_glfb_has_gpu_fence()))
            kl_glfb_present(out);
    }
    return OVRP_SUCCESS;
}

// The two named entry points over the body above, for the same reason
// BeginFrame/BeginFrame4 are separate: the work list counts by the name the call
// actually hit.
static uint64_t klovrp_EndFrame(int guest_frame_index) {
    ovrp_hit("ovrp_EndFrame");
    // No layer list on this path — 1.28's legacy VRDevice hands its eye
    // textures down through ovrp_SetupEyeTexture2 and never describes a layer,
    // so it can only ever have rendered into the whole thing.
    return klovrp_end_frame_impl(guest_frame_index, NULL, NULL, -1);
}

// ovrp_EndFrame4(frameIndex, layerSubmits, layerSubmitCount, sync) — the 1.40
// shape (real 0x16ed40, which -1001s only when layerSubmits is NULL *and* the
// count is non-zero, so an empty submission is legal). The layer list is the
// guest naming which layers it just filled; we know which stage it drew into
// from the observation window that BeginFrame opened, which is a stronger
// statement than the list (it is what the GL side actually saw), so the list is
// not what the STAGE comes from.
//
// It is read for one thing, and it is the only place that thing is stated: the
// per-eye **ViewportRect**, i.e. how much of the eye texture the guest actually
// drew into this frame. See kl_ovrp_render_pose.viewport — a title lowering its
// render resolution does it here, not by resizing anything.
//
// **On this path `frameIndex % stages` is NOT the stage**, and the report says so
// loudly: a 9000-frame 1.40 run counts 8786 "the guest's frame index disagreed".
// That is not a fault. The XR-SDK display provider rotates its own TextureStage
// ring and the index it passes here has no relation to it, where 1.28's legacy
// VRDevice derived the stage from exactly this counter (and measured 0
// disagreements). The observation is authoritative either way — the same run
// reports 0 frames drawn into no stage, 0 into several and 0 off-thread — so the
// counter is only ever the fallback for runs with no GL observation at all.
// The guest's submitted-frame counter — see KL_OVRP_POKE and kl_vulkan_capture_eyes.
static unsigned g_vk_frame;

static uint64_t klovrp_EndFrame4(int guest_frame_index, const void *layer_submits,
                                 int layer_submit_count, void *sync) {
    ovrp_hit("ovrp_EndFrame4");
    (void)sync;
    if (!layer_submits && layer_submit_count) return OVRP_FAIL_INVALID_PARAM;
    klovrp_census_submits(layer_submits, layer_submit_count);
    // The list of everything that is NOT the eye layer, for the compositors.
    // Before this, all of it was dropped here.
    klovrp_record_overlays(layer_submits, layer_submit_count);
    int vp[8], of[2] = { 0, 0 };
    int have = klovrp_submit_viewports(layer_submits, layer_submit_count, vp, of);
    // The guest's own answer to "which stage did I just draw?", at submit+0x04.
    // -1 when there is no eye layer in the list to read it from, which is a
    // different thing from stage 0 and has to stay distinguishable — the frame
    // record's fallback ladder tests it (klovrp_end_frame_impl).
    int named = klovrp_submit_stage(layer_submits, layer_submit_count);
    uint64_t r = klovrp_end_frame_impl(guest_frame_index, have ? vp : NULL,
                                       have ? of : NULL, named);

    // BONELAB / the Vulkan path: this call IS the guest's assertion that it has
    // finished drawing the eye textures, so it is where they can be read back
    // and where a compositor is told the picture is ready.
    //
    // g_vk_frame is defined down with KL_OVRP_POKE, which reads it: a poke
    // aimed at `f9900` and the capture named `vk_f09900_*.png` have to be the
    // same frame.
    //
    // The stage comes out of the submit rather than from
    // kl_ovrp_last_complete_stage(), and that is not a shortcut — the latter is
    // derived from GL draw observation (kl_glfb_render_stages), which sees
    // nothing at all on a Vulkan guest.
    if (kl_vulkan_guest_active()) {
        // File-scope rather than a static in here, because KL_OVRP_POKE's
        // frame form is timed against THIS number: a poke aimed at `f9900`
        // has to mean the same frame as `vk_f09900_*.png`, or a script written
        // by looking at the captures presses at some other screen.
        kl_vulkan_capture_eyes(g_vk_frame++, named < 0 ? 0 : named);
        // After the capture, not before: the capture's own submit is work on
        // the same queue, and a compositor let loose between the two would race
        // it. Ordinary runs write no captures at all, so this is the only
        // frame-completion point either way.
        kl_vulkan_frame_done(named < 0 ? 0 : named);
    }
    return r;
}

int kl_ovrp_stage_render_pose(int stage, kl_ovrp_render_pose *out) {
    if ((unsigned)stage >= KLOVRP_MAX_STAGES || !out) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int have = g_frames.r[stage].serial != 0;
    if (have) *out = g_frames.r[stage];
    pthread_mutex_unlock(&g_frames.mu);
    return have;
}

// The association's health, live rather than at the end of the run.
//
// It belongs in the report too, but a device run normally ends by the immersive
// space being invalidated rather than by the guest finishing, so the report is
// the one thing that often does not get written. These go in the compositor's
// 2-second line, which always does.
void kl_ovrp_association_stats(uint64_t *dropped, uint64_t *multi,
                               uint64_t *cross, uint64_t *disagree) {
    pthread_mutex_lock(&g_frames.mu);
    if (dropped)  *dropped  = g_frames.unobserved;
    if (multi)    *multi    = g_frames.multi;
    if (cross)    *cross    = g_frames.cross_thread;
    if (disagree) *disagree = g_frames.stage_disagree;
    pthread_mutex_unlock(&g_frames.mu);
}

int kl_ovrp_last_complete_stage(void) {
    pthread_mutex_lock(&g_frames.mu);
    int s = g_frames.last_complete;
    pthread_mutex_unlock(&g_frames.mu);
    return s;
}

// --- the same two moments, for a guest that never speaks OVRPlugin ----------
//
// See kl_ovrp.h for why these exist. They write the same records the pair above
// writes and share every static with them, deliberately: the compositor reads
// one ring, and a second one filled by a second guest is how the two answers
// drift. No process runs both — a run is Beat Saber or it is Steam Link.
void kl_ovrp_frame_begin_external(void) {
    // klovrp_head() rather than klovrp_step_head(): the step ring is libunity's
    // Update/Render split, which has no counterpart here. The latch is the same
    // one, taken by kl_ovrp_frame_latch() at xrWaitFrame just before this.
    klovrp_pose h = klovrp_head();
    pthread_mutex_lock(&g_frames.mu);
    uint64_t s = ++g_frames.serial;
    kl_ovrp_render_pose *r = &g_frames.pending;
    r->px = h.px; r->py = h.py; r->pz = h.pz;
    r->qx = h.qx; r->qy = h.qy; r->qz = h.qz; r->qw = h.qw;
    // Through klovrp_eye_tan, NOT g_eye_tan: this is what the composite
    // builds its quad from, so it has to be the frustum the guest was
    // actually told to render with, unified or not.
    for (int e = 0; e < 2; e++)
        memcpy(r->tangents[e], klovrp_eye_tan(e), sizeof r->tangents[e]);
    r->serial = s;
    r->stage = -1;
    r->complete = 0;
    g_frames.pending_index = (int)s;
    pthread_mutex_unlock(&g_frames.mu);
}

void kl_ovrp_frame_end_external(int stage, const float *pose7, const float *tan8) {
    if ((unsigned)stage >= KLOVRP_MAX_STAGES) {
        // Not clamped. A stage outside the ring means the swapchain has more
        // images than the record ring has slots, and folding it onto a slot
        // that already describes a different picture is the exact pose/picture
        // mismatch this ring exists to prevent — better to file nothing and say
        // so than to file the wrong thing.
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] external frame stage %d is beyond the %d-slot "
                            "record ring — this frame's pose is NOT filed\n",
                    stage, KLOVRP_MAX_STAGES);
        }
        return;
    }
    // Louder than a comment, once: the compositor's own per-stage report walks
    // 0..kl_ovrp_stage_count(), so a stage past that is composited correctly
    // and reported as if it did not exist.
    if (stage >= kl_ovrp_stage_count()) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] external frame stage %d is beyond "
                            "KL_OVRP_STAGES (%d) — it composites, but the "
                            "per-stage report will not show it\n",
                    stage, kl_ovrp_stage_count());
        }
    }
    pthread_mutex_lock(&g_frames.mu);
    if (g_frames.serial) {
        // The guest's own statement of what it drew, where it has one. See the
        // header: this is what keeps a compositor from correcting a delta the
        // guest has already corrected.
        // ...and by how much, which is the measurement the whole change rests
        // on rather than an assertion about it. A guest that renders at the
        // pose it was handed submits that same pose and this is 0; a streaming
        // client that warps the host's frame to the predicted display pose
        // before submitting it reports the frame's own head motion, and THAT is
        // the delta a compositor must not apply a second time. Printed on the
        // first frame and then rarely, because the number only has to settle
        // which of the two guests this is.
        if (pose7) {
            klovrp_pose a = { .qx = g_frames.pending.qx, .qy = g_frames.pending.qy,
                              .qz = g_frames.pending.qz, .qw = g_frames.pending.qw };
            klovrp_pose b = { .qx = pose7[3], .qy = pose7[4],
                              .qz = pose7[5], .qw = pose7[6] };
            float deg = klovrp_quat_degrees(&a, &b);
            float dx = pose7[0] - g_frames.pending.px;
            float dy = pose7[1] - g_frames.pending.py;
            float dz = pose7[2] - g_frames.pending.pz;
            static unsigned n;
            static float worst_deg, worst_m;
            float m = sqrtf(dx * dx + dy * dy + dz * dz);
            if (deg > worst_deg) worst_deg = deg;
            if (m > worst_m) worst_m = m;
            if (n == 0 || ++n % 600 == 0)
                fprintf(stderr, "  [ovrp] the guest submitted a pose %.3f deg / "
                                "%.4f m from the one it was handed (worst %.3f deg "
                                "/ %.4f m) — it %s\n", (double)deg, (double)m,
                        (double)worst_deg, (double)worst_m,
                        worst_deg > 0.05f || worst_m > 0.002f
                            ? "REPROJECTS ITS OWN FRAMES, so ours must not repeat it"
                            : "renders at the pose it is given");
            if (n == 0) n = 1;
            g_frames.pending.px = pose7[0];
            g_frames.pending.py = pose7[1];
            g_frames.pending.pz = pose7[2];
            g_frames.pending.qx = pose7[3];
            g_frames.pending.qy = pose7[4];
            g_frames.pending.qz = pose7[5];
            g_frames.pending.qw = pose7[6];
        }
        if (tan8) memcpy(g_frames.pending.tangents, tan8, sizeof g_frames.pending.tangents);
        g_frames.pending.stage = stage;
        g_frames.pending.complete = 1;
        g_frames.r[stage] = g_frames.pending;
        g_frames.last_complete = stage;
        g_frames.filed[stage]++;
    }
    pthread_mutex_unlock(&g_frames.mu);
}

// --- M7: the two hands ------------------------------------------------------
// Node ids and enum values are not invented: they are read out of the guest's
// own global-metadata.dat (the OVRPlugin C# it was compiled against):
//   Node:      EyeLeft=0 EyeRight=1 EyeCenter=2 HandLeft=3 HandRight=4
//              TrackerZero=5..TrackerThree=8 Head=9
//   Controller: LTouch=1 RTouch=2 Touch=3 Remote=4 Gamepad=0x10
//               LHand=0x20 RHand=0x40 Hands=0x60 Active=0x80000000
// Default hand poses ride a head-relative offset (resolved in
// klovrp_GetNodePoseState), so controllers render somewhere sensible before any
// frontend starts driving them — and stay in the frustum wherever the head is.
// These were absolute tracking-space coordinates until the FloorLevel origin
// was measured, which put them below the floor and out of view.
static int g_hand_set[2];

// Buttons/touches are ovrpButton/ovrpTouch bit values straight from the
// metadata (Button: One=1 Two=2 Start=0x100 PrimaryIndexTrigger=0x2000
// PrimaryHandTrigger=0x4000 PrimaryThumbstick=0x8000 ..., and the Secondary*
// block at <<20-ish for the other hand — the frontend hands us final bits).
typedef struct {
    uint32_t buttons, touches, neartouches;
    float    index_trigger, hand_trigger, stick_x, stick_y;
} klovrp_input_state;
static klovrp_input_state g_input[2];

void kl_ovrp_set_hand_motion(int hand, float px, float py, float pz,
                             float qx, float qy, float qz, float qw,
                             float vx, float vy, float vz,
                             float avx, float avy, float avz) {
    if ((unsigned)hand > 1) return;
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, vx, vy, vz, avx, avy, avz, 1 };
    // A measured velocity is authoritative — nothing is derived here. The
    // history still advances, so a publisher that later drops to the pose-only
    // call differentiates against the right previous sample instead of a gap.
    klovrp_hist_note(&g_hand_hist[hand], &v);
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
}

// ...and the pose-only form, which is what the macOS viewer publishes. It used
// to hand this straight to the call above with six zeros, i.e. "this controller
// is stationary" about one that is moving. It differentiates now.
void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    if ((unsigned)hand > 1) return;
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0 };
    klovrp_derive_motion(&v, &g_hand_hist[hand]);
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
}

// Is the velocity in the latched hand sample a measurement, or is it the
// "we do not know" that OpenXR spells velocityFlags == 0?
int kl_ovrp_hand_motion_known(int hand) {
    if ((unsigned)hand > 1) return 0;
    return g_hand_pose[hand].motion_valid;
}

void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y) {
    if ((unsigned)hand > 1) return;
    g_input[hand].buttons = buttons;
    g_input[hand].touches = touches;
    g_input[hand].neartouches = touches;   // capacitive proximity ~ touch
    g_input[hand].index_trigger = index_trigger;
    g_input[hand].hand_trigger = hand_trigger;
    g_input[hand].stick_x = stick_x;
    g_input[hand].stick_y = stick_y;
}

// --- KL_OVRP_POKE: a scripted controller sequence ---------------------------
//
// `KL_OVRP_POKE="12:A,15:A,17:DOWN,18:A"` — at 12 s press A, and so on. What it
// buys is a run that drives ITSELF past a title screen and into a menu, which is
// the difference between a bug being reproducible on this machine and only being
// reachable by a person wearing a headset.
//
// Merged at the point the guest READS its controller state rather than written
// into g_input, for two reasons: a frontend publishing every frame would
// otherwise overwrite it between the press and the read, and applied here it
// works with **no frontend at all** — a headless `./build/m_boot re4` gets the
// same presses as the viewer. That is KL_VIEW_POKE's reasoning (driven from the
// pump, not from a window) in the seam this guest's input actually arrives
// through: RE4 reads OVRPlugin, not the NDK input queue.
//
// The clock starts at the guest's FIRST read of controller state, not at
// process start. Startup here is dominated by the engine's one-time shader
// optimization, which is minutes and is not the same length twice, so wall
// clock from exec would aim every press at a different screen.
// `f9900:A` presses at the guest's 9900th submitted frame instead, and that
// form is the one to write scripts in: it is the SAME number `KL_VK_OUT` puts
// in `vk_f09900_*.png`, so a sequence worked out by looking at the captures
// lands on the screens they show. Seconds cannot do that here — startup is
// dominated by the engine's one-time shader optimization, which is minutes and
// is not the same length twice.
#define KLOVRP_POKE_MAX 32
static struct klovrp_poke {
    double   t;                       // seconds after the first read
    long     frame;                   // ...or the guest frame, when >= 0
    uint32_t b[2];                    // raw button bits, per hand
    float    idx[2], grip[2];         // triggers, per hand
    float    sx, sy;                  // right thumbstick
    int      fired;
} g_poke[KLOVRP_POKE_MAX];
static int    g_poke_n = -1;          // -1 = not parsed yet
static double g_poke_hold = 0.25;     // seconds a press is held
static double g_poke_t0;

// One name to one control. Right hand unless the name says otherwise, which is
// OVRPlugin's own convention: A/B are the right controller's face buttons and
// X/Y the left's, so a table keyed on the name cannot put them on one hand.
static int klovrp_poke_name(const char *n, size_t len, struct klovrp_poke *p) {
    struct { const char *n; int hand; uint32_t bit; float idx, grip, sx, sy; } t[] = {
        { "A",     1, KL_OVRP_RAW_A,     0, 0, 0, 0 },
        { "B",     1, KL_OVRP_RAW_B,     0, 0, 0, 0 },
        { "X",     0, KL_OVRP_RAW_X,     0, 0, 0, 0 },
        { "Y",     0, KL_OVRP_RAW_Y,     0, 0, 0, 0 },
        { "START", 1, KL_OVRP_RAW_START, 0, 0, 0, 0 },
        { "BACK",  1, KL_OVRP_RAW_BACK,  0, 0, 0, 0 },
        { "RTRIG", 1, KL_OVRP_RAW_RINDEX_TRIGGER, 1, 0, 0, 0 },
        { "LTRIG", 0, KL_OVRP_RAW_LINDEX_TRIGGER, 1, 0, 0, 0 },
        { "RGRIP", 1, KL_OVRP_RAW_RHAND_TRIGGER,  0, 1, 0, 0 },
        { "LGRIP", 0, KL_OVRP_RAW_LHAND_TRIGGER,  0, 1, 0, 0 },
        { "UP",    1, KL_OVRP_RAW_RTHUMBSTICK_UP,    0, 0,  0,  1 },
        { "DOWN",  1, KL_OVRP_RAW_RTHUMBSTICK_DOWN,  0, 0,  0, -1 },
        { "LEFT",  1, KL_OVRP_RAW_RTHUMBSTICK_LEFT,  0, 0, -1,  0 },
        { "RIGHT", 1, KL_OVRP_RAW_RTHUMBSTICK_RIGHT, 0, 0,  1,  0 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (strlen(t[i].n) != len || strncasecmp(t[i].n, n, len) != 0) continue;
        p->b[t[i].hand] |= t[i].bit;
        if (t[i].idx)  p->idx[t[i].hand]  = 1.0f;
        if (t[i].grip) p->grip[t[i].hand] = 1.0f;
        if (t[i].sx)   p->sx = t[i].sx;
        if (t[i].sy)   p->sy = t[i].sy;
        return 1;
    }
    return 0;
}

static void klovrp_poke_parse(void) {
    g_poke_n = 0;
    const char *s = getenv("KL_OVRP_POKE");
    if (!s || !*s) return;
    g_poke_hold = kl_env_int("KL_OVRP_POKE_HOLD_MS", 250) / 1000.0;
    while (*s && g_poke_n < KLOVRP_POKE_MAX) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        int by_frame = (*s == 'f' || *s == 'F');
        if (by_frame) s++;
        char *end = NULL;
        double t = strtod(s, &end);
        if (end == s || *end != ':') {
            fprintf(stderr, "  [ovrp] KL_OVRP_POKE: expected <seconds>:<BUTTON> or "
                            "f<frame>:<BUTTON> at \"%s\" — the rest of the script "
                            "is ignored\n", s);
            return;
        }
        struct klovrp_poke p;
        memset(&p, 0, sizeof p);
        p.t = by_frame ? 0 : t;
        p.frame = by_frame ? (long)t : -1;
        s = end + 1;
        int ok = 1;
        for (;;) {
            const char *w = s;
            while (*s && *s != ',' && *s != '+' && *s != ' ') s++;
            // Named, not skipped: a misspelt button is a press that never
            // happens, and a script that silently does less than it says
            // reads as the guest ignoring input.
            if (!klovrp_poke_name(w, (size_t)(s - w), &p)) {
                fprintf(stderr, "  [ovrp] KL_OVRP_POKE: no button named \"%.*s\" — "
                                "A B X Y START BACK RTRIG LTRIG RGRIP LGRIP UP "
                                "DOWN LEFT RIGHT\n", (int)(s - w), w);
                ok = 0;
            }
            if (*s != '+') break;
            s++;
        }
        if (ok) g_poke[g_poke_n++] = p;
    }
    if (g_poke_n)
        fprintf(stderr, "  [ovrp] KL_OVRP_POKE: %d press(es) scripted, %.0f ms each, "
                        "timed from the guest's first controller read\n",
                g_poke_n, g_poke_hold * 1000.0);
}

// This hand's state as the guest should see it: what a frontend published, plus
// whatever the script is holding down right now.
static klovrp_input_state klovrp_input(int hand) {
    klovrp_input_state v = g_input[hand];
    if (g_poke_n < 0) klovrp_poke_parse();
    if (g_poke_n == 0) return v;
    double now = klovrp_mono_now();
    if (g_poke_t0 == 0.0) g_poke_t0 = now;
    double t = now - g_poke_t0;
    unsigned frame = __atomic_load_n(&g_vk_frame, __ATOMIC_RELAXED);
    for (int i = 0; i < g_poke_n; i++) {
        struct klovrp_poke *p = &g_poke[i];
        if (p->frame >= 0) {
            // A hold in FRAMES for a press timed in frames: the frame rate here
            // is whatever the host manages, so a press held for a quarter of a
            // second could be one frame or forty.
            long held = (long)(g_poke_hold * 90.0);
            if ((long)frame < p->frame || (long)frame > p->frame + held) continue;
        } else if (t < p->t || t > p->t + g_poke_hold) {
            continue;
        }
        if (!p->fired) {
            p->fired = 1;
            fprintf(stderr, "  [ovrp] KL_OVRP_POKE at %.1f s / frame %u: "
                            "buttons L=%#x R=%#x\n", t, frame, p->b[0], p->b[1]);
        }
        v.buttons    |= p->b[hand];
        v.touches    |= p->b[hand];
        v.neartouches |= p->b[hand];
        if (p->idx[hand] > v.index_trigger) v.index_trigger = p->idx[hand];
        if (p->grip[hand] > v.hand_trigger) v.hand_trigger = p->grip[hand];
        if (hand == 1) {
            if (p->sx != 0) v.stick_x = p->sx;
            if (p->sy != 0) v.stick_y = p->sy;
        }
    }
    return v;
}

// --- ...and the read side, for kl_openxr.c (kl_ovrp.h documents the contract)
//
// The pose comes from klovrp_hand(), which is the PINNED one — the same value
// ovrp_GetNodePoseState hands the other guest in the same frame. Reading
// g_hand_pose directly here would have been the shorter line and would have
// reintroduced §12.19's unlatched read in a new API.
int kl_ovrp_hand_motion(int hand, float *pos, float *quat, float *vel, float *ang) {
    if ((unsigned)hand > 1) return 0;
    klovrp_pose p = klovrp_hand(hand);
    if (pos)  { pos[0] = p.px; pos[1] = p.py; pos[2] = p.pz; }
    if (quat) { quat[0] = p.qx; quat[1] = p.qy; quat[2] = p.qz; quat[3] = p.qw; }
    if (vel)  { vel[0] = p.vx; vel[1] = p.vy; vel[2] = p.vz; }
    if (ang)  { ang[0] = p.avx; ang[1] = p.avy; ang[2] = p.avz; }
    return __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE) ? 1 : 0;
}

int kl_ovrp_controller_input(int hand, uint32_t *buttons, uint32_t *touches,
                             float *index_trigger, float *hand_trigger,
                             float *stick_x, float *stick_y) {
    if ((unsigned)hand > 1) return 0;
    klovrp_input_state in = klovrp_input(hand);
    if (buttons)       *buttons = in.buttons;
    if (touches)       *touches = in.touches;
    if (index_trigger) *index_trigger = in.index_trigger;
    if (hand_trigger)  *hand_trigger = in.hand_trigger;
    if (stick_x)       *stick_x = in.stick_x;
    if (stick_y)       *stick_y = in.stick_y;
    // Presence is the pose seam's, not this one's: a frontend that publishes a
    // hand publishes both in the same breath, and there is no separate "the
    // buttons are meaningful" signal here. A hand-tracked hand therefore reads
    // present with every button released, which is the truth about it.
    return __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE) ? 1 : 0;
}

// KL_OVRP_HANDS_SWEEP=1: collapse both hands onto the head and sweep their
// pitch from -70 to +70 degrees in 5-degree steps, holding each step long
// enough for the KL_OVRP_FAKE_TRIGGER duty cycle to complete two presses.
//
// It exists because the ray a controller casts is NOT the pose we report:
// Beat Saber's IVRPlatformHelper.AdjustControllerTransform rotates the
// controller transform by a device-specific offset (a Touch controller does
// not point along its tracked forward axis), and that offset is game data we
// cannot read. A sweep does not need to know it — if any pitch produces a UI
// hit, the offset is the only unknown left; if none does over 140 degrees, the
// ray is not the problem and the controller never reaches the raycaster.
// Pair it with KL_OVRP_FAKE_TRIGGER=1 and watch for the menu advancing.
// KL_OVRP_DUMP_VRDEVICE=1: dump libunity's own Oculus VRDevice object once.
// The pointer lives at a fixed vaddr in this build (libunity+0x127a6c0 — the
// `ldr x8, [x?, #1728]` every VRDevice method starts with), and its first three
// words are the unique device ids libunity stamps into both the joystick
// descriptors (0x9bd3fc/0x9bd60c) and the XR node states (0x9bbf60..0x9bbf98):
// [+0] left controller, [+4] right controller, [+8] HMD. Zero ids mean Unity
// never allocated the controller devices, which is the difference between "the
// game ignores our controllers" and "there are no controllers to ignore".
// Read-only, and only when asked for — this is a build-specific address.
static void klovrp_dump_vrdevice(void) {
    static int done;
    if (done) return;
    if (!kl_env_on("KL_OVRP_DUMP_VRDEVICE", 0)) { done = 1; return; }
    kl_image *img = kl_find_image("libunity.so");
    if (!img) { fprintf(stderr, "  [ovrp] VRDevice: no libunity image\n"); done = 1; return; }
    unsigned char *base = kl_base(img);
    if (!base || kl_span(img) < 0x127a6c8) {
        fprintf(stderr, "  [ovrp] VRDevice: base=%p span=0x%zx\n", (void *)base, kl_span(img));
        done = 1; return;
    }
    void *obj = *(void **)(base + 0x127a000 + 1728);
    if (!obj) return;                    // not built yet — try again next frame
    done = 1;
    const uint32_t *w = obj;
    fprintf(stderr, "  [ovrp] VRDevice %p: idLeft=%u idRight=%u idHmd=%u\n",
            obj, w[0], w[1], w[2]);
    // Name every function-pointer slot by matching it against what we handed
    // back from kl_ovrp_sym. This is the VRDevice's whole contract with the
    // plugin in one place: which entry point sits behind each `ldr x8, [x?,
    // #N] / blr x8` in the disassembly, so a status predicate we answer wrong
    // can be found by name instead of by chasing offsets.
    void *const *slot = (void *const *)obj;
    for (size_t off = 0; off + 8 <= 768; off += 8) {
        void *fn = slot[off / 8];
        if (!fn) continue;
        for (unsigned i = 0; i < g_nsym; i++)
            if (g_sym[i].ptr == fn) {
                fprintf(stderr, "  [ovrp]   +%-4zu %s\n", off, g_sym[i].name);
                break;
            }
    }
}

static void klovrp_hand_sweep(const klovrp_pose *head, klovrp_pose *hand) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_HANDS_SWEEP", 0);
    if (!on) return;
    static unsigned polls;
    // ~2 hand-pose polls a frame from libunity's node loop, so 512 polls is
    // ~256 frames a step — two full presses of the trigger's 64-frame period.
    // 29 steps is ~7400 frames, so a long run sweeps more than once and at
    // least one full sweep happens after the menu is up.
    unsigned step = (polls++ / 512u) % 29u;
    float deg = -70.0f + 5.0f * (float)step;
    static unsigned last_step = ~0u;
    if (step != last_step) {
        last_step = step;
        fprintf(stderr, "  [ovrp] hand sweep: pitch %+.0f deg\n", (double)deg);
    }
    float half = deg * 0.5f * 3.14159265f / 180.0f;
    float sx = sinf(half), cw = cosf(half);
    // q = q_head ⊗ q_pitch, with q_pitch a rotation about the head's local X.
    hand->qx = head->qw * sx + head->qx * cw;
    hand->qy = head->qy * cw + head->qz * sx;
    hand->qz = head->qz * cw - head->qy * sx;
    hand->qw = head->qw * cw - head->qx * sx;
    hand->px = head->px; hand->py = head->py; hand->pz = head->pz;
}

// out-struct via the sret register x8, NOT x2: the real function returns the
// 88-byte ovrpPoseStatef by value (its own prologue does `mov x19, x8` and an
// 0x58-byte memcpy back), so callers place the destination in x8 and the
// declared args are just (w0=step, w1=nodeId). x8 must be captured before any
// call — see kl_ovrp_sret.S for why the entry point is an assembly thunk and
// `out` arrives here as an ordinary parameter. Fields libunity consumes:
// +0x00 quat xyzw (w at +0x0c), +0x10 position, +0x1c velocity,
// +0x28 acceleration, +0x34 angular velocity, +0x40 angular acceleration.
// Node 9 (Head) and the eyes/trackers report the frontend head pose; nodes
// 3/4 (HandLeft/HandRight) report their own poses.
uint64_t klovrp_GetNodePoseState_impl(int step, int node, void *out) {
    ovrp_hit("ovrp_GetNodePoseState");
    ovrp_log_arg("ovrp_GetNodePoseState", node, __builtin_return_address(0));
    memset(out, 0, 0x58);
    // The pose for the STEP the guest asked about, which is the sample it took
    // at ovrp_Update2 — not a live global re-read twenty times a frame. The
    // argument was being discarded; see klovrp_Update2.
    klovrp_pose head = klovrp_step_head(step);
    const klovrp_pose *p = &head;
    klovrp_pose hand, eye;
    if (node == 0 || node == 1) {
        // EyeLeft/EyeRight. The head pose displaced by this eye's own offset,
        // rotated into the world by the head's orientation — this pair IS the
        // guest's IPD (kl_ovrp.h). Node 2 (EyeCenter) and node 9 (Head) keep
        // the head pose itself, which is what they mean.
        float dx, dy, dz, ox, oy, oz;
        klovrp_eye_offset(node, &dx, &dy, &dz);
        klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
        // ...and this eye's own orientation, not the head's. The eye is TURNED
        // on this display (see kl_ovrp_set_eye_rotation), and the frustum we
        // report for it through ovrp_GetNodeFrustum2 is expressed in that
        // turned frame — so a node pose carrying the head's orientation
        // describes an eye that does not exist, and the guest renders the right
        // cone of directions pointing the wrong way.
        eye = klovrp_eye_cant() ? klovrp_qmul(&head, g_eye_rot[node]) : head;
        eye.px = head.px + ox; eye.py = head.py + oy; eye.pz = head.pz + oz;
        p = &eye;
    } else if (node == 3 || node == 4) {
        int h = node - 3;
        // KL_OVRP_HANDS_IN_VIEW=1: park both hands at a fixed spot well inside
        // the *current head's* frustum, overriding whatever the frontend last
        // wrote. Answers one question and only one — does the guest draw
        // controllers at all? Read side, so it beats the viewer's per-frame
        // writes. Diagnostic: the hands do not move with your real hands.
        //
        // This offset is head-relative on purpose. It used to be absolute
        // tracking-space coordinates chosen for an identity head, and the guest
        // asks for a FloorLevel origin — so the "in view" park was 12 cm below
        // the floor and 1.7 m under the head, i.e. reliably out of frustum.
        // The knob meant to prove the controllers render was hiding them.
        static int inview = -1;
        if (inview < 0) inview = kl_env_on("KL_OVRP_HANDS_IN_VIEW", 0);
        if (inview || !__atomic_load_n(&g_hand_set[h], __ATOMIC_ACQUIRE)) {
            float dx = inview ? (h ? 0.15f : -0.15f) : (h ? 0.20f : -0.20f);
            float dy = inview ? -0.12f : -0.30f;
            float dz = inview ? -0.55f : -0.35f;
            float ox, oy, oz;
            klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
            hand = head;
            hand.px = head.px + ox;
            hand.py = head.py + oy;
            hand.pz = head.pz + oz;
        } else {
            hand = klovrp_step_hand(step, h);
        }
        klovrp_dump_vrdevice();
        klovrp_hand_sweep(&head, &hand);
        p = &hand;
    }
    float *f = out;
    f[0] = p->qx; f[1] = p->qy;      // quat xyz at +0x00
    f[2] = p->qz; f[3] = p->qw;      // quat w at +0x0c
    f[4] = p->px; f[5] = p->py;      // position at +0x10
    f[6] = p->pz;
    // Velocity at +0x1c and angular velocity at +0x34, both of which were left
    // zero until now. libunity copies all four vectors straight into its XR
    // node state, so a zero here is not "unknown", it is "not moving" — and
    // Unity's own code differentiates nothing, it trusts what the plugin says.
    // Acceleration (+0x28) and angular acceleration (+0x40) stay zero: no
    // tracker on this platform reports them, and a difference-of-differences
    // estimate off a 90 Hz pose stream is noise wearing a physical name.
    f[7]  = p->vx;  f[8]  = p->vy;  f[9]  = p->vz;
    f[13] = p->avx; f[14] = p->avy; f[15] = p->avz;
    return OVRP_SUCCESS;
}

// The 1.40 wrapper's pose query — ovrp_GetNodePoseState3. This is NOT a
// renamed alias of the un-numbered form: the real plugin (0x16f680) reads its
// out pointer from x3, and the wrapper (OculusSystem::GetNodePoseState, a
// MEMBER function, this in x0) hard-codes w0 = -1 before the call — its own
// args land in w1/w2/x3 and its body never forwards them. So the entry has
// four arguments and cannot share the sret x8 capture of the un-numbered
// shape. The 0x58-byte struct it copies is the SAME layout
// klovrp_GetNodePoseState_impl writes; w0=-1 is "the most recent step",
// which klovrp_step_ix maps to the non-render buffer and falls back to the
// live pose { klovrp_head } if never sampled, so -1 is safe. w2 is the NODE
// under the member-fn layout (w1=step, w2=node, x3=out) and the first-call
// log prints all three data args so a run settles that mapping.
static uint64_t klovrp_GetNodePoseState3(int a, int b, int c, void *out) {
    ovrp_hit("ovrp_GetNodePoseState3");
    if (!out) return -1001;
    {
        static int logged;
        if (!logged) {
            logged = 1;
            fprintf(stderr, "  [ovrp] GetNodePoseState3 first call: "
                            "w0(step)=-1 compiled in, w1=%d, w2=%d (candidate node)\n",
                    b, c);
        }
    }
    return klovrp_GetNodePoseState_impl(a, c, out);
}

// The wait for a frame to be beginable. Real 0x16ec30 is a scalar INT return
// (the frame index), not ovrpResult + out*: not-init answers -1002, and on
// success it clamps any negative internal result to 0. Each call issues the
// NEXT frame id, which the guest feeds to klovrp_BeginFrame, where it is
// threaded to g_frames.pending_index — the guest's own frame counter, which
// the record ring is sized to multiplex (see KLOVRP_STAGES_DEFAULT). Nobody
// actually waits here BY DEFAULT: for a Unity guest this call happens inside a
// frame the driver already initiated, so the driver is the clock and a wait
// here would only be this call delaying its own caller.
//
// A guest that owns its frame loop is the other shape, and then this is the
// only point at which a display can say "not yet" — kl_ovrp_set_frame_pacer is
// that, and it is the OVRPlugin half of kl_openxr's pacer. Unset everywhere
// else, so nothing about the Unity path changes.
static void (*g_frame_pacer)(void);
void kl_ovrp_set_frame_pacer(void (*wait)(void)) { g_frame_pacer = wait; }

static uint64_t klovrp_WaitToBeginFrame(void) {
    ovrp_hit("ovrp_WaitToBeginFrame");
    if (g_frame_pacer) {
        g_frame_pacer();
        // Here rather than at BeginFrame, and for the reason kl_ovrp_frame_latch
        // gives: the pose has to be pinned before ANYTHING in the frame can ask
        // for one, and this call is the frame's first instruction. A guest that
        // asks between the two would see the compositor's newer pose and draw
        // from a pose the record does not carry.
        kl_ovrp_frame_latch();
    }
    static uint64_t next;   // first issued id is 1 — the caller's 0 is identity
    return next++;
}

// ovrpVector3f by value — a 12-byte HFA, so the floats go home in s0..s2,
// which a uint64 return would never set. Returning a three-float struct from
// C makes Clang emit exactly the real plugin's `ldp s0, s1 / ldr s2 / ret`.
// Zeros are also what the real plugin returns when no boundary exists.
typedef struct { float x, y, z; } klovrp_vec3;
static klovrp_vec3 klovrp_GetBoundaryDimensions(void) {
    ovrp_hit("ovrp_GetBoundaryDimensions");
    return (klovrp_vec3){0, 0, 0};
}

// Controller masks (metadata OVRPlugin.Controller): LTouch=1, RTouch=2,
// Touch=3, Remote=4, Gamepad=0x10, LHand=0x20, RHand=0x40, Hands=0x60,
// Active=0x80000000. Only the Touch controllers exist here, so a mask asking
// for anything else (Remote/Gamepad/Hands) connects nothing. Active expands
// to the controllers that are present.
#define OVRP_CTRL_LTOUCH 0x1u
#define OVRP_CTRL_RTOUCH 0x2u
#define OVRP_CTRL_ACTIVE 0x80000000u

// ovrpControllerState prefix, shared by all three versions (field order from
// the guest's own metadata — OVRInput.ControllerState{,2,4}):
//   +0x00 u32 ConnectedControllers   +0x10 f32 LIndexTrigger
//   +0x04 u32 Buttons                +0x14 f32 RIndexTrigger
//   +0x08 u32 Touches                +0x18 f32 LHandTrigger
//   +0x0C u32 NearTouches            +0x1C f32 RHandTrigger
//   +0x20 vec2 LThumbstick           +0x28 vec2 RThumbstick
// v2 appends +0x30/+0x38 vec2 L/RTouchpad; v4 appends +0x40.. bytes
// L/RBatteryPercentRemaining, L/RRecenterCount, then 28 reserved.
// KL_OVRP_FAKE_BUTTONS=<hex>: OR this mask into both hands' Buttons and
// Touches, toggling on/off on a duty cycle so GetDown/GetUp transitions
// actually occur — a held-from-boot bit never reads as a press.
//
// Which bits reach the game, measured from libunity's joystick fill
// (0x9bd338 left / 0x9bd548 right) against this title's InputManager axes:
//   Buttons 0x1 (A)   -> joystick button 0  = MenuButtonRightHand
//   Buttons 0x2 (B)   -> joystick button 1
//   Buttons 0x100 (X) -> joystick button 2  = MenuButtonLeftHand
//   Buttons 0x200 (Y) -> joystick button 3
//   Buttons 0x400 (LThumbstick) -> button 8 = MenuButtonLeftHandOculusTouch
//   Buttons 0x4   (RThumbstick) -> button 9 = MenuButtonRightHandOculusTouch
//   Buttons 0x100000 (Start)    -> button 7
// Every other bit — including the raw trigger bits 0x04000000/0x08000000/
// 0x10000000/0x20000000 — is read by nobody: libunity carries the triggers as
// *float axes*, never as buttons. So this knob cannot produce a UI click, and
// a "nothing reacts with 0xffffffff" result says nothing about the trigger.
// That is what KL_OVRP_FAKE_TRIGGER below is for.
static unsigned klovrp_fake_phase(void) {
    // ~7 controller polls a frame, so 256 on / 256 off is roughly a 0.4 s
    // press at 90 Hz — slow enough for a UI to see both edges.
    static unsigned calls;
    return (calls++ >> 8) & 1;
}

static uint32_t klovrp_fake_buttons(void) {
    static int init;
    static uint32_t mask;
    if (!init) {
        init = 1;
        mask = kl_env_uint("KL_OVRP_FAKE_BUTTONS", 0);
    }
    if (!mask) return 0;
    return klovrp_fake_phase() ? mask : 0;
}

// KL_OVRP_FAKE_TRIGGER=<0..1>: drive both index triggers to this value on the
// same duty cycle. This is the click, not a button: Beat Saber's
// VRControllersInputManager reads Input.GetAxis("TriggerLeftHand"/"...Right"),
// which the InputManager asset binds to joystick axes 8 and 9 — libunity fills
// those from LIndexTrigger/RIndexTrigger, the floats. It is the only way to
// exercise a menu click without the interactive viewer.
static float klovrp_fake_trigger(void) {
    static int init;
    static float v;
    if (!init) {
        init = 1;
        v = kl_env_on("KL_OVRP_FAKE_TRIGGER", 0) ? 1.0f : 0.0f;
    }
    if (v <= 0.0f) return 0.0f;
    return klovrp_fake_phase() ? v : 0.0f;
}

static void fill_controller_state(int mask, void *out, int version) {
    memset(out, 0, version == 4 ? 0x60 : version == 2 ? 0x40 : 0x30);
    uint32_t m = (uint32_t)mask;
    if (m & OVRP_CTRL_ACTIVE) m |= OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH;
    uint32_t conn = m & (OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH);
    uint32_t fake = conn ? klovrp_fake_buttons() : 0;
    float fake_trig = conn ? klovrp_fake_trigger() : 0.0f;
    uint8_t *b = out;
    uint32_t *w = (uint32_t *)out;
    float *f = (float *)out;
    // Through klovrp_input, so a KL_OVRP_POKE script reaches the OVRPlugin
    // guest and the OpenXR one identically. Read once per hand per call: the
    // script is timed, so two reads inside one fill could straddle a press's
    // end and hand the guest a button that is down in Buttons and up in Touches.
    klovrp_input_state in[2] = { klovrp_input(0), klovrp_input(1) };
    w[0] = conn;                                        // ConnectedControllers
    if (conn & OVRP_CTRL_LTOUCH) {
        w[1] |= in[0].buttons;                          // Buttons
        w[2] |= in[0].touches;                          // Touches
        w[3] |= in[0].neartouches;                      // NearTouches
        f[4] = in[0].index_trigger;                     // LIndexTrigger
        f[6] = in[0].hand_trigger;                      // LHandTrigger
        f[8] = in[0].stick_x; f[9] = in[0].stick_y;
        if (version >= 2) { f[12] = in[0].stick_x; f[13] = in[0].stick_y; }
        if (version >= 4) b[0x40] = 100;                // LBatteryPercentRemaining
    }
    if (conn & OVRP_CTRL_RTOUCH) {
        w[1] |= in[1].buttons;
        w[2] |= in[1].touches;
        w[3] |= in[1].neartouches;
        f[5] = in[1].index_trigger;                     // RIndexTrigger
        f[7] = in[1].hand_trigger;                      // RHandTrigger
        f[10] = in[1].stick_x; f[11] = in[1].stick_y;
        if (version >= 2) { f[14] = in[1].stick_x; f[15] = in[1].stick_y; }
        if (version >= 4) b[0x41] = 100;                // RBatteryPercentRemaining
    }
    w[1] |= fake;                                       // Buttons
    w[2] |= fake;                                       // Touches
    if (fake_trig > 0.0f) {
        if (conn & OVRP_CTRL_LTOUCH) f[4] = fake_trig;  // LIndexTrigger
        if (conn & OVRP_CTRL_RTOUCH) f[5] = fake_trig;  // RIndexTrigger
    }
}

// 64-byte ovrpControllerState2 by value via x8 (real plugin: stp q0..q3 of
// zeros on the failure path).
uint64_t klovrp_GetControllerState2_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState2");
    ovrp_log_arg("ovrp_GetControllerState2", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 2);
    return OVRP_SUCCESS;
}

// Same state, newest shape: (mask=w0, out=x1), 0x60 bytes (real plugin's
// memcpy size), plain ovrpResult. This is the one the game itself polls
// (OVRP_1_16_0::ovrp_GetControllerState4 via P/Invoke).
static uint64_t klovrp_GetControllerState4(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState4");
    ovrp_log_arg("ovrp_GetControllerState4", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 4);
    return OVRP_SUCCESS;
}

// Same shape, 48 bytes (real plugin: three q-stores). v1 of the above; this
// is the one libunity's legacy VRDevice polls once a frame with mask=Touch.
uint64_t klovrp_GetControllerState_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState");
    ovrp_log_arg("ovrp_GetControllerState", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 1);
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// M8 — haptics: the seam running the OTHER way
//
// Every other entry point here answers a question the guest asked. These three
// are the guest acting on the hardware, so the seam inverts: the guest fills a
// queue, the frontend drains it once a frame (kl_ovrp_haptics_pull).
//
// This title drives the BUFFERED path, not the one-shot
// OVRInput.SetControllerVibration: its global-metadata.dat carries OVRHaptics /
// OVRHapticsClip / OVRHapticsOutput and the whole OVRHaptics.Config property
// set, which is Oculus's sample-stream API.
//
//   ovrp_GetControllerHapticsDesc  -- how fast, how wide, how deep. Read ONCE,
//                                     at OVRHaptics's static init.
//   ovrp_GetControllerHapticsState -- how much is queued, how much room is
//                                     left. Every frame, per hand.
//   ovrp_SetControllerHaptics      -- N amplitude samples.
//
// A zeroed descriptor is not a neutral answer. OVRHapticsOutput sizes its
// native buffer at MaximumBufferSamplesCount, paces to keep
// OptimalBufferSamplesCount queued, and clamps to the SamplesAvailable
// reported; zeroed descriptor plus zeroed state is a coherent "this controller
// cannot vibrate", and the managed side queues clips into a zero-length buffer
// and never calls SetControllerHaptics at all.
//
// The numbers are Touch's — the controller claimed everywhere else
// (Build.MODEL, ovrp_GetSystemHeadsetType): 320 Hz, one unsigned byte per
// sample, buffers of 1..256 with 20 the pace target and 5 the run-dry mark.
// A Sense controller's real capabilities would be more literally true and would
// put Unity's pacing maths somewhere this title has never been.
//
// Reading the two struct layouts the right way round is the one ABI claim here
// rather than a measurement. With the descriptor answered, SetControllerHaptics
// SamplesCount lands at or below OptimalBufferSamplesCount (20); a transposed
// descriptor shows up immediately as counts of 1 or 256. The state struct fails
// SILENTLY instead — swap SamplesAvailable and SamplesQueued and the guest
// computes "0 samples of room" forever and sends nothing — so it carries
// KL_HAPTICS_SWAP_STATE=1 as its A/B. KL_HAPTICS_TRACE=1 prints both halves.
#define KLOVRP_HAP_RATE     320        // samples per second
#define KLOVRP_HAP_MAX      256        // MaximumBufferSamplesCount, and our ring
#define KLOVRP_HAP_OPTIMAL  20         // OptimalBufferSamplesCount
#define KLOVRP_HAP_SAFE     5          // MinimumSafeSamplesQueued
#define KLOVRP_HAP_MINBUF   1          // MinimumBufferSamplesCount

// --- The playback model, and the one this replaced ------------------------
//
// **This is an envelope follower, not a pulse batcher, and the difference was
// measured on hardware.** The first version tried to hand the frontend whole
// pulses: hold samples back until at least 32 ms of them had accumulated
// (ALVR's floor — an actuator cannot say anything in 10 ms), then emit one
// event at the PEAK of that span. Two things were wrong with it, and both were
// obvious the moment a Sense controller was in hand:
//
//  1. **The threshold could never be met.** `OVRHapticsOutput` runs in
//     low-latency mode: it keeps `MinimumSafeSamplesQueued` plus one frame's
//     worth queued — 5 + 4 = **9 samples, 28 ms** — and tops up a few samples
//     per frame. 28 ms is less than the 32 ms we were waiting for, so the span
//     test essentially never fired and emission fell to the tie-break case
//     ("the guest fed nothing since the last pull"), which is a race between
//     the guest's frame rate and the compositor's.
//  2. **Worse, the wait lost the samples.** Retirement ran on the clock and
//     simply dropped what it retired, so a clip could be entirely consumed by
//     the queue's own drain before the hold ever released it. A note cut is
//     ~100 ms; most of it evaporated, and what came out was the tail — a blip,
//     or nothing at all. Held blades still buzzed because that stream never
//     stops, so the race fires often enough to feel continuous. That exactly
//     matches what the headset reported: constant vibration fine, note cuts
//     blippy with no decay, and sometimes silent.
//
// So retirement IS playback now. A sample that comes due is a sample the hand
// should be feeling, and `klovrp_hap_drain` accumulates what it retires into a
// level the frontend reads each frame. Nothing is buffered on our side and
// nothing can be dropped; the shape of the clip survives at whatever rate the
// frontend asks (90 Hz against a 320 Hz envelope, so ~3 samples a window).
//
// The floor stays, because ALVR's reason for it stands — "controllers can't do
// 10ms vibrations" — but it is now a floor on how long a level is HELD, not on
// how long we wait before reporting one. A 10 ms burst is reported at once and
// then held for the rest of the 32 ms, which is a drive the actuator can act
// on rather than a request it ignores.
#define KLOVRP_HAP_MIN_ON   0.032f     // KL_HAPTICS_MIN_MS
#define KLOVRP_HAP_MAX_S    0.5f       // the longest window we will describe

// Deltas only, never compared against anything outside this file, so which
// Darwin monotonic clock this is does not matter — CACurrentMediaTime() on the
// frontend side is a different epoch and is deliberately not mixed in.
static double klovrp_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// One ring per hand. The guest pushes from its own thread, the frontend pulls
// from the render thread, and GetControllerHapticsState is read from a third,
// so the mutex is not defensive.
static struct klovrp_haptics {
    pthread_mutex_t mu;
    uint8_t  s[KLOVRP_HAP_MAX];
    int      head;          // index of the next sample due to play
    int      count;         // samples queued, playing forward from head
    double   t_head;        // monotonic instant at which s[head] plays
    // What has come due since the frontend last looked. Written by the drain —
    // which runs from the guest's own state queries as well as from the pull,
    // so it must accumulate rather than overwrite — and consumed by the pull.
    float    level;         // peak of those samples, 0..1
    double   last_pull;     // so the pull can say what window its level covers
    // ALVR's floor, as a hold rather than a wait. See the model comment above.
    float    held;
    double   hold_until;
    // The legacy vibration path, kept SEPARATE from the ring above rather than
    // folded into it. It has to be: this title calls
    // ovrp_SetControllerVibration(mask, 0, 0) on both hands EVERY FRAME — an
    // idle "nothing should be buzzing" — while OVRHaptics feeds the ring from
    // the same managed frame. Serving the stop by clearing the queue would let
    // one path silently erase the other's clip sixty times a second, and the
    // symptom would be haptics that are merely intermittent rather than absent.
    float    vib_amp;       // 0 = not vibrating
    double   vib_until;     // when this vibration lapses if not refreshed
    // ...and the OpenXR path (xrApplyHapticFeedback), kept separate from BOTH
    // of the above for the same reason they are separate from each other. It is
    // the same shape as the vibration pair — a level with a lapse — but a
    // different owner, and a shared slot is how one owner's stop erases the
    // other's buzz with no error anywhere. A run only ever has one of the three
    // live; the cost of keeping them apart is two floats.
    float    xr_amp;
    double   xr_until;
    uint64_t pushes, samples, pulses;
    float    peak;
} g_hap[2] = {
    { .mu = PTHREAD_MUTEX_INITIALIZER }, { .mu = PTHREAD_MUTEX_INITIALIZER },
};

static int klovrp_hap_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_HAPTICS_TRACE", 0);
    return on;
}

// Retire the samples whose moment has passed — **which is the same thing as
// playing them**, so their peak is accumulated into `level` on the way out
// rather than discarded. That equivalence is the whole model: the queue drains
// on the wall clock at exactly the rate we told the guest it would (so
// OVRHapticsOutput's own per-frame prediction of SamplesQueued agrees with what
// we report), and every sample it drains is one the hand should have felt.
//
// Runs from the pull AND from the guest's own state queries, twice a frame, so
// it accumulates into `level` and only the pull clears it.
//
// Call with the lock held.
static void klovrp_hap_drain(struct klovrp_haptics *h, double now) {
    if (h->count <= 0) { h->count = 0; h->t_head = now; return; }
    double due = (now - h->t_head) * KLOVRP_HAP_RATE;
    if (due < 1.0) return;
    int adv = (int)due;
    if (adv > h->count) adv = h->count;
    // The PEAK across the retired run, not its mean: what a hand feels across a
    // few milliseconds is the attack, and averaging one over a window that
    // includes the silence before it turns every sharp cut into a soft push.
    for (int i = 0; i < adv; i++) {
        float v = h->s[(h->head + i) % KLOVRP_HAP_MAX] / 255.0f;
        if (v > h->level) h->level = v;
    }
    h->head = (h->head + adv) % KLOVRP_HAP_MAX;
    h->count -= adv;
    h->t_head += (double)adv / KLOVRP_HAP_RATE;
    if (h->count == 0) h->t_head = now;
}

// Which hands a controller mask names. Active means "whatever is connected",
// and we report both Touch controllers connected, so it means both.
static int klovrp_hap_hands(int mask) {
    uint32_t m = (uint32_t)mask;
    if (m & OVRP_CTRL_ACTIVE) m |= OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH;
    // LHand/RHand (0x20/0x40) name the hand-tracking "controllers" the same
    // enum carries; they are the same two hands to us.
    int hands = 0;
    if (m & (OVRP_CTRL_LTOUCH | 0x20u)) hands |= 1;
    if (m & (OVRP_CTRL_RTOUCH | 0x40u)) hands |= 2;
    return hands;
}

// Returns how many of the n samples were TAKEN. Every caller but one ignores
// that — the buffered API tells the guest how much room there is beforehand and
// a shortfall there means it ignored the answer. The PCM API below is the one
// where a partial take is the normal case and has to be reported back.
static int klovrp_hap_enqueue(int hand, const uint8_t *s, int n) {
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);
    int dropped = 0;
    for (int i = 0; i < n; i++) {
        if (h->count >= KLOVRP_HAP_MAX) { dropped = n - i; break; }
        h->s[(h->head + h->count) % KLOVRP_HAP_MAX] = s[i];
        h->count++;
        if (s[i] / 255.0f > h->peak) h->peak = s[i] / 255.0f;
    }
    h->pushes++;
    h->samples += (uint64_t)(n - dropped);
    pthread_mutex_unlock(&h->mu);
    if (dropped && klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: hand %d queue full, dropped %d "
                        "sample(s) — the guest ignored SamplesAvailable\n",
                hand, dropped);
    return n - dropped;
}

// Drop whatever is still queued for a hand. The PCM API's Append=false means
// "this is a new stream", which is a REPLACE and not an append — a clip that
// starts while the last one is still draining must not be heard behind it.
static void klovrp_hap_clear(int hand) {
    struct klovrp_haptics *h = &g_hap[hand];
    pthread_mutex_lock(&h->mu);
    h->count = 0;
    h->t_head = klovrp_mono();
    pthread_mutex_unlock(&h->mu);
}

// (mask = w0, HapticsBuffer by value = x1/x2). The buffer is
// { void *Samples; int SamplesCount; } — 16 bytes, so AAPCS64 passes it in two
// registers rather than by reference, which is why the count arrives as a
// second scalar and not through a pointer. Returns ovrpBool, not ovrpResult
// (managed OVRPlugin tests it against Bool.True).
static uint64_t klovrp_SetControllerHaptics(int mask, const void *samples,
                                            uint64_t count) {
    ovrp_hit("ovrp_SetControllerHaptics");
    int n = (int)(uint32_t)count;
    if (!samples || n <= 0) return 1;
    if (n > KLOVRP_HAP_MAX) n = KLOVRP_HAP_MAX;   // the guest was told the ceiling
    int hands = klovrp_hap_hands(mask);
    // The whole buffer's shape, not just its first byte. The open question this
    // answers: does this title's note-cut clip carry a DECAY, or is it a square
    // burst whose fade on a Quest is the LRA's own ring-down? The two want
    // different things from a Sense controller, and one line of trace settles
    // it — a decaying clip prints a falling row, a square one a flat row.
    if (klovrp_hap_trace()) {
        const uint8_t *s = samples;
        int lo = 255, hi = 0;
        for (int i = 0; i < n; i++) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        fprintf(stderr, "  [ovrp] haptics: SetControllerHaptics(mask=0x%x) %d "
                        "sample(s), %u..%u:", (unsigned)mask, n, lo, hi);
        for (int i = 0; i < n && i < 32; i++) fprintf(stderr, " %u", s[i]);
        fprintf(stderr, "%s\n", n > 32 ? " ..." : "");
    }
    if (hands & 1) klovrp_hap_enqueue(0, samples, n);
    if (hands & 2) klovrp_hap_enqueue(1, samples, n);
    return 1;
}

// --- The PCM path, which is 1.40's and is a THIRD producer for this queue ----
//
// Beat Saber 1.40 ships `libhaptics_sdk.so` — Meta's Haptics SDK, a Rust engine
// with its own player thread — and that library does not go through OVRHaptics
// at all. It dlopens libOVRPlugin.so and resolves exactly two entry points by
// name (they are in its strings, beside "Initializing OVRPlugin backend...",
// "Starting playback at Hz on location" and "Only samples were consumed!"):
// ovrp_GetControllerSampleRateHz and ovrp_SetControllerHapticsPcm. Both were
// resolved and neither was implemented, so the FIRST haptic effect the game
// plays — the pulse a menu button makes when the pointer highlights it — hit
// the abort-by-name path, on the SDK's own thread. That reads as "the viewer
// crashes when you hover a button" and names nothing about haptics.
//
// **The signatures are read out of the real libOVRPlugin.so in this APK**, not
// transcribed from a header, because a shim entry is an unchecked contract
// (trap 6b) and this one carries two pointers:
//
//   ovrp_GetControllerSampleRateHz  (+0x16fef0): w0 = controller, x1 = float*.
//       `cbz x1` -> -1001, so the out pointer is x1 and nothing else is read.
//   ovrp_SetControllerHapticsPcm    (+0x16fd70): w0 = controller, x1 = a
//       32-byte struct BY REFERENCE (AAPCS64: composites over 16 bytes are
//       passed as a pointer to a copy). It tests [x1+0x8] and [x1+0x18] for
//       NULL and copies the whole 32 bytes with `ldp q0,q1,[x1]`, which fixes
//       the layout as
//           +0x00 uint32 BufferSize     (+4 padding)
//           +0x08 const float *Buffer
//           +0x10 float  SampleRateHz
//           +0x14 ovrpBool Append
//           +0x18 uint32 *SamplesConsumed
//       — the C# HapticsPcmVibration, field for field.
//
// Two things make this fold into the existing queue rather than needing a
// fourth source next to vib_amp/xr_amp (kl_ovrp.h): the samples are the same
// quantity — an amplitude envelope — and the RATE is ours to choose, because
// the SDK asks for it first and then sends at whatever we answered. Answering
// KLOVRP_HAP_RATE makes the resample below an identity in the case that
// actually happens, and keeps one playback model for all of them.
#define KLOVRP_PCM_MAX 4000            // OVRP_MAX_HAPTICS_PCM_BUFFER_SIZE

static uint64_t klovrp_GetControllerSampleRateHz(int controller, float *out) {
    ovrp_hit("ovrp_GetControllerSampleRateHz");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    (void)controller;                            // one controller model, both hands
    *out = (float)KLOVRP_HAP_RATE;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_SetControllerHapticsPcm(int controller, const void *vib) {
    ovrp_hit("ovrp_SetControllerHapticsPcm");
    if (!vib) return OVRP_FAIL_INVALID_PARAM;
    const uint8_t *v = vib;
    uint32_t      n_in     = *(const uint32_t *)(v + 0x00);
    const float  *buffer   = *(const float *const *)(v + 0x08);
    float         rate     = *(const float *)(v + 0x10);
    int           append   = v[0x14] != 0;       // ovrpBool; a byte either way
    uint32_t     *consumed = *(uint32_t *const *)(v + 0x18);
    // The real plugin refuses both of these outright, so a caller cannot be
    // relying on either being optional.
    if (!buffer || !consumed) return OVRP_FAIL_INVALID_PARAM;
    if (n_in > KLOVRP_PCM_MAX) n_in = KLOVRP_PCM_MAX;

    // Ring slots per input sample. The SDK asked us for the rate and sends at
    // it, so this is 1.0 in every run that has happened; the mapping exists so
    // that a caller which sends at some other rate is played at the right SPEED
    // rather than at the wrong one silently.
    if (!(rate > 0.0f)) rate = (float)KLOVRP_HAP_RATE;
    uint8_t  ring[KLOVRP_HAP_MAX];
    long     n_ring = lrintf((float)n_in * (float)KLOVRP_HAP_RATE / rate);
    if (n_ring > KLOVRP_HAP_MAX) n_ring = KLOVRP_HAP_MAX;
    if (n_ring < 0) n_ring = 0;
    for (long j = 0; j < n_ring; j++) {
        long  i = (long)((double)j * rate / KLOVRP_HAP_RATE);
        if (i >= (long)n_in) i = (long)n_in - 1;
        float a = i >= 0 ? buffer[i] : 0.0f;
        a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
        ring[j] = (uint8_t)lrintf(a * 255.0f);
    }

    int hands = klovrp_hap_hands(controller);
    if (!hands) { *consumed = 0; return OVRP_SUCCESS; }
    if (!append) {
        if (hands & 1) klovrp_hap_clear(0);
        if (hands & 2) klovrp_hap_clear(1);
    }
    // Both hands take the same buffer, so what was consumed is what the
    // FULLER queue could take — reporting the other hand's would ask the guest
    // to re-send samples one hand already has.
    int took = (int)n_ring;
    if (hands & 1) { int t = klovrp_hap_enqueue(0, ring, (int)n_ring); if (t < took) took = t; }
    if (hands & 2) { int t = klovrp_hap_enqueue(1, ring, (int)n_ring); if (t < took) took = t; }

    // Back in the caller's OWN sample units, not ring slots. The SDK logs
    // "Only N samples were consumed!" and re-sends the rest, so this number is
    // load-bearing rather than informational — and taking the whole buffer is
    // reported as the whole buffer rather than as whatever the rate conversion
    // rounds to. The live rate is 2000 Hz in batches of 72 against our 320, so
    // one ring slot is 6.25 of the caller's samples and the rounding is worth
    // several of them; a short answer there asks it to re-send samples the hand
    // has already been given.
    uint32_t took_in = (took >= n_ring)
        ? n_in
        : (uint32_t)((double)took * rate / KLOVRP_HAP_RATE + 0.5);
    if (took_in > n_in) took_in = n_in;
    *consumed = took_in;

    if (klovrp_hap_trace()) {
        static int said;
        float lo = 1.0f, hi = 0.0f;
        for (uint32_t i = 0; i < n_in; i++) {
            if (buffer[i] < lo) lo = buffer[i];
            if (buffer[i] > hi) hi = buffer[i];
        }
        fprintf(stderr, "  [ovrp] haptics: SetControllerHapticsPcm(ctrl=0x%x) %u "
                        "sample(s) @ %.1f Hz, %s, %.2f..%.2f -> %ld ring, %u consumed%s\n",
                (unsigned)controller, n_in, (double)rate,
                append ? "append" : "replace", (double)lo, (double)hi, n_ring, took_in,
                said++ ? "" : "  <- libhaptics_sdk's PCM path, 1.40's own");
    }
    return OVRP_SUCCESS;
}

// The legacy level API: (mask = w0, frequency = s0, amplitude = s1), ovrpBool.
// A vibration set here runs until it is changed — OVRInput's own contract is
// that a caller which means to sustain one keeps calling — so what is recorded
// is a level and a lapse time, not a finite buffer.
//
// **Measured on this title: it arrives with amplitude 0 on both hands every
// single frame** (3138 calls across 3000 frames), i.e. as a per-frame "stop"
// rather than as the way anything is actually played. That is what forced the
// two sources apart; see the `vib_amp` comment on the struct.
//
// KL_HAPTICS_VIB_LAPSE=<seconds> is the ceiling on an un-refreshed vibration,
// default 2 s. The real API has one of about that; ours matters more, because a
// frontend that has already been handed a pulse cannot be told to stop.
//
// The frequency argument is dropped, deliberately. OVRPlugin's 0..1 is a
// selector between two fixed Touch motor rates; a Sense controller's second
// axis is CoreHaptics *sharpness*, which is not the same quantity, and there is
// no measurement here that would justify a mapping between them.
static void klovrp_vibration(int mask, float frequency, float amplitude) {
    int hands = klovrp_hap_hands(mask);
    static float lapse = -1.0f;
    if (lapse < 0.0f) lapse = kl_env_float("KL_HAPTICS_VIB_LAPSE", 2.0f);
    float a = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude;
    double now = klovrp_mono();
    for (int hand = 0; hand < 2; hand++) {
        if (!(hands & (1 << hand))) continue;
        struct klovrp_haptics *h = &g_hap[hand];
        pthread_mutex_lock(&h->mu);
        float was = h->vib_amp;
        h->vib_amp = a;
        // Refreshing pushes the lapse out; it does not re-trigger anything.
        // The pull reads this as a level, so a caller re-asserting it every
        // frame and one asserting it once behave identically.
        h->vib_until = a > 0.0f ? now + lapse : 0.0;
        pthread_mutex_unlock(&h->mu);
        // Traced on the EDGE only. The per-frame idle stop above would
        // otherwise bury every other line in this subsystem.
        if (klovrp_hap_trace() && (was > 0.0f) != (a > 0.0f))
            fprintf(stderr, "  [ovrp] haptics: hand %d vibration %s "
                            "(freq=%.2f, amp=%.2f)\n",
                    hand, a > 0.0f ? "on" : "off",
                    (double)frequency, (double)a);
    }
}

// ...and the two ABIs over it. The un-suffixed form answers ovrpBool and the
// ...2 form (real body +0x2f164) answers ovrpResult — trap 10's whole shape, so
// they are two wrappers rather than one table entry listed twice. The ARGUMENTS
// are identical: +0x2f164 forwards only w0 and leaves s0/s1 exactly where the
// caller put them, so the frequency and amplitude reach the backend untouched.
static uint64_t klovrp_SetControllerVibration(int mask, float frequency,
                                              float amplitude) {
    ovrp_hit("ovrp_SetControllerVibration");
    klovrp_vibration(mask, frequency, amplitude);
    return OVRP_TRUE;
}
static uint64_t klovrp_SetControllerVibration2(int mask, float frequency,
                                               float amplitude) {
    ovrp_hit("ovrp_SetControllerVibration2");
    klovrp_vibration(mask, frequency, amplitude);
    return OVRP_SUCCESS;
}

// ovrpHapticsState is { int SamplesAvailable; int SamplesQueued; } — 8 bytes,
// so it comes home in x0 with SamplesAvailable in the low word. This used to
// sit in the answer-zero list, which reads as "no room, nothing queued": the
// managed side clamps what it sends to SamplesAvailable, so zero room meant it
// never sent anything at all.
static uint64_t klovrp_hap_state_packed(int mask) {
    int hands = klovrp_hap_hands(mask);
    int hand = (hands & 1) ? 0 : 1;          // one hand per call, left first
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);
    int queued = h->count;
    pthread_mutex_unlock(&h->mu);
    int available = KLOVRP_HAP_MAX - queued;

    static int swap = -1;
    if (swap < 0) swap = kl_env_on("KL_HAPTICS_SWAP_STATE", 0);
    uint32_t lo = swap ? (uint32_t)queued : (uint32_t)available;
    uint32_t hi = swap ? (uint32_t)available : (uint32_t)queued;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint64_t klovrp_GetControllerHapticsState(int mask) {
    ovrp_hit("ovrp_GetControllerHapticsState");
    return klovrp_hap_state_packed(mask);
}

// 24-byte ovrpHapticsDesc by value via x8. See the block comment above for why
// every field of this matters and why zeroing it silenced the whole path.
static void klovrp_hap_desc_fill(void *out) {
    int32_t *d = out;
    d[0] = KLOVRP_HAP_RATE;                      // SampleRateHz
    d[1] = 1;                                    // SampleSizeInBytes
    d[2] = KLOVRP_HAP_SAFE;                      // MinimumSafeSamplesQueued
    d[3] = KLOVRP_HAP_MINBUF;                    // MinimumBufferSamplesCount
    d[4] = KLOVRP_HAP_OPTIMAL;                   // OptimalBufferSamplesCount
    d[5] = KLOVRP_HAP_MAX;                       // MaximumBufferSamplesCount
}

uint64_t klovrp_GetControllerHapticsDesc_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerHapticsDesc");
    (void)mask;                                  // one controller model, both hands
    klovrp_hap_desc_fill(out);
    return OVRP_SUCCESS;
}

// ...and the ...2 forms of both, which UE4 is the first guest here to call.
// Same questions, different ABI, and the difference is exactly trap 10b: where
// the un-suffixed pair return the struct (the desc through x8, the 8-byte state
// in x0), these take an OUT POINTER and return ovrpResult. Read out of RE4's
// own libOVRPlugin.so (+0x2f1a8 and +0x2f1e8): both `cbz` the pointer for
// -1001, take -1002 with no singleton, and otherwise TAIL-CALL the same backend
// method the un-suffixed form reaches — so the values are the same values, and
// answering them from anywhere but the shared body would be two descriptions of
// one controller.
//
// A struct-returning implementation under these names would have left the
// caller's buffer untouched and answered a success code, which is the shape
// that cost `ovrp_GetVersion2` a session: a strlen of a stack slot nobody
// wrote, naming nothing.
static uint64_t klovrp_GetControllerHapticsDesc2(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerHapticsDesc2");
    (void)mask;                                  // one controller model, both hands
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    klovrp_hap_desc_fill(out);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetControllerHapticsState2(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerHapticsState2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    uint64_t packed = klovrp_hap_state_packed(mask);
    int32_t *s = out;
    s[0] = (int32_t)(uint32_t)packed;            // SamplesAvailable
    s[1] = (int32_t)(uint32_t)(packed >> 32);    // SamplesQueued
    return OVRP_SUCCESS;
}

static float klovrp_hap_min_on(void) {
    static float s = -1.0f;
    if (s < 0.0f) s = kl_env_float("KL_HAPTICS_MIN_MS", KLOVRP_HAP_MIN_ON * 1000.0f) / 1000.0f;
    return s;
}

// The OpenXR guest's order, in. See kl_ovrp.h for why this is a third source
// rather than a reuse of the vibration slot.
//
// The clamp on the duration is a real limit, not defensiveness: OpenXR lets an
// app ask for a vibration lasting minutes, and nothing here can be told to stop
// once a frontend has been handed a level — so an unbounded one is a controller
// that buzzes until the process ends. KL_HAPTICS_XR_MAX caps it.
void kl_ovrp_haptics_apply(int hand, float amplitude, float seconds) {
    if ((unsigned)hand > 1) return;
    static float cap = -1.0f;
    if (cap < 0.0f) cap = kl_env_float("KL_HAPTICS_XR_MAX", 5.0f);
    float a = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude;
    float s = seconds;
    if (!(s > 0.0f)) s = klovrp_hap_min_on();   // XR_MIN_HAPTIC_DURATION: a click
    if (s < klovrp_hap_min_on()) s = klovrp_hap_min_on();
    if (s > cap) s = cap;

    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    float was = h->xr_amp;
    h->xr_amp = a;
    h->xr_until = a > 0.0f ? now + (double)s : 0.0;
    pthread_mutex_unlock(&h->mu);
    if (klovrp_hap_trace() && (was > 0.0f) != (a > 0.0f))
        fprintf(stderr, "  [ovrp] haptics: hand %d xrApplyHapticFeedback %s "
                        "(amp=%.2f, %.0f ms)\n",
                hand, a > 0.0f ? "on" : "off", (double)a, (double)s * 1000.0);
}

void kl_ovrp_haptics_stop(int hand) {
    if ((unsigned)hand > 1) return;
    struct klovrp_haptics *h = &g_hap[hand];
    pthread_mutex_lock(&h->mu);
    int was = h->xr_amp > 0.0f;
    h->xr_amp = 0.0f;
    h->xr_until = 0.0;
    pthread_mutex_unlock(&h->mu);
    if (was && klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: hand %d xrStopHapticFeedback\n", hand);
}

int kl_ovrp_haptics_pull(int hand, float *amplitude, float *seconds) {
    if ((unsigned)hand > 1) return 0;
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);

    float amp = h->level;
    h->level = 0.0f;
    // The level API covers the same window. Taken as a maximum rather than as
    // an alternative: they are two descriptions of one actuator, and the louder
    // of two simultaneous claims is the safe merge. In practice only one is
    // ever live — every vibration call this title makes is a stop.
    if (h->vib_amp > 0.0f) {
        if (now >= h->vib_until) h->vib_amp = 0.0f;      // lapsed, unrefreshed
        else if (h->vib_amp > amp) amp = h->vib_amp;
    }
    // The OpenXR path, merged the same way. Its lapse is the duration the guest
    // asked for rather than a ceiling on an un-refreshed level, so an
    // xrApplyHapticFeedback that is never stopped still ends on time.
    if (h->xr_amp > 0.0f) {
        if (now >= h->xr_until) h->xr_amp = 0.0f;
        else if (h->xr_amp > amp) amp = h->xr_amp;
    }

    // ALVR's floor: an actuator cannot act on a burst shorter than about 32 ms,
    // so a level that appears is held for at least that long instead of being
    // reported once and dropped. Beat Saber's note cut is longer than this, so
    // the hold only ever extends the tail of one — it does not invent a buzz.
    if (amp > 0.0f) {
        h->held = amp;
        h->hold_until = now + klovrp_hap_min_on();
    } else if (now < h->hold_until) {
        amp = h->held;
    }

    // The window this level describes. The frontend needs it to know how long
    // to drive for if it cannot follow a level continuously.
    float span = (float)(h->last_pull > 0 ? now - h->last_pull : klovrp_hap_min_on());
    if (span > KLOVRP_HAP_MAX_S) span = KLOVRP_HAP_MAX_S;
    if (span <= 0.0f) span = klovrp_hap_min_on();
    h->last_pull = now;
    if (amp > 0.0f) h->pulses++;
    pthread_mutex_unlock(&h->mu);

    if (amp <= 0.0f) return 0;
    if (amplitude) *amplitude = amp;
    if (seconds) *seconds = span;
    if (klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: hand %d level %.2f over %.0f ms\n",
                hand, (double)amp, (double)span * 1000.0);
    return 1;
}

// ovrpResult with a bool OUT-PARAM (OVRP_1_18_0), NOT the bare ovrpBool its
// name-mates ovrp_GetAppHasVrFocus/ovrp_GetUserPresent use. It sat in the
// bool-yes list returning 1, and managed OVRPlugin reads that as
//
//     Result result = ovrp_GetAppHasInputFocus(out inputFocus);
//     if (Result.Success == result) return inputFocus == Bool.True;
//     return false;                       // <- 1 is not Success (0)
//
// so `OVRPlugin.hasInputFocus` was false while we believed we were answering
// yes. Beat Saber hides its menu controllers while input focus is away — it
// has inputFocusWasCaptured/Released events for exactly this — which switched
// off the MenuControllers object and with it both saber hilts, both lasers and
// every UI raycast. hasVrFocus stayed true throughout, which is what made the
// pair look healthy.
//
// Safe to give an out-param because ONLY libil2cpp references this name;
// libunity does not (checked against both binaries). ovrp_GetAppHasVrFocus is
// referenced by both and really is a bare bool, so it stays where it is.
static uint64_t klovrp_GetAppHasInputFocus(char *out) {
    ovrp_hit("ovrp_GetAppHasInputFocus");
    if (out) *out = 1;
    return OVRP_SUCCESS;
}

// ovrpResult with a bool OUT-PARAM (real plugin: ldrb/str w8 to [x0],
// -1001 on NULL). We accepted ovrp_SetAppAsymmetricFov at init, so the
// read-back says enabled — one device story, like the headset above.
static uint64_t klovrp_GetAppAsymmetricFov(char *out) {
    ovrp_hit("ovrp_GetAppAsymmetricFov");
    *out = 1;
    return OVRP_SUCCESS;
}

// (float *freqs, int *count) -> ovrpResult. The real 0x1704c0 null-checks the
// SECOND argument (-1001) and passes the first through untested, which is the
// two-phase query the managed side makes: once with freqs = NULL to learn the
// count, then again with a buffer that size. Success is 0 like every other
// entry point here (`csel w0, w0, wzr, lt`) — this returned the COUNT until
// now, and a positive ovrpResult is not success.
static uint64_t klovrp_GetSystemDisplayAvailableFrequencies(float *buf, int *count) {
    ovrp_hit("ovrp_GetSystemDisplayAvailableFrequencies");
    if (!count) return -1001;
    // One rate, and it is the one we report as current. Offering a menu of
    // frequencies we cannot actually switch between would invite the guest to
    // ask for one, and klovrp_SetSystemDisplayFrequency would then have to
    // refuse it — a list is a promise, so it says exactly what we can present.
    if (buf) buf[0] = kl_ovrp_display_frequency();
    *count = 1;
    return OVRP_SUCCESS;
}

// ovrpResult with an enum OUT-PARAM. ovrpXrApiType: 0 Unknown, 1 Oculus
// (legacy VrApi), 2 OpenXR. This APK's plugin is the VrApi build — no OpenXR
// string exists anywhere in it — so 1 is the only coherent answer.
static uint64_t klovrp_GetNativeXrApiType(int *out) {
    ovrp_hit("ovrp_GetNativeXrApiType");
    *out = 1;
    return OVRP_SUCCESS;
}

// The two-attempt contract (libunity 0x9bbb48/0x9bbb9c): attempt 1 passes
// handle=0 and must FAIL; the retry passes Unity's own GL texture name
// (measured: 0x18/0x1a — the very names the eye FBOs then attach). Storage
// for that name arrives from nowhere else: Unity never calls
// glTexImage2D/glTexStorage2D for the eye color textures in the whole trace,
// and the real plugin cannot (it imports no storage calls — its VrApi does
// it internally). So the plugin's job on the retry is to allocate storage
// for the texture Unity hands down, on the context current right here
// (Unity's render thread — this call arrives inside
// IVRDeviceCallback_CreateEyeTextureResources). fmt=2 maps to sRGB, matching
// the eye-sized color textures Unity allocates for itself (0x8c43).

// GL_RGBA16F: Unity renders the scene into an RGBA16F MSAA renderbuffer
// (measured: fmt 0x881a, samples=4, via the blit probe), and ES 3.0 makes a
// float->unorm blit INVALID_OPERATION — the eye texture must be float to
// match. This is what the guest's fmt=2 means in practice.
#define KL_OVRP_TEXFMT_EYE  0x881A
static uint64_t klovrp_SetupEyeTexture2(int eye, int stage, uintptr_t handle,
                                        int w, int h, int depth, int fmt, void *ctx) {
    ovrp_hit("ovrp_SetupEyeTexture2");
    if (!handle) return 0;
    static void (*gl_BindTexture)(uint32_t, uint32_t);
    static void (*gl_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
    if (!gl_BindTexture) {
        gl_BindTexture   = kl_egl_sym("glBindTexture");
        gl_TexStorage2D  = kl_egl_sym("glTexStorage2D");
    }
    fprintf(stderr, "  [ovrp] SetupEyeTexture2(eye=%d stage=%d) -> tex=%zu %dx%d\n",
            eye, stage, (size_t)handle, w, h);
    // The capture reads the frame back from the FBO this texture is attached
    // to — tell kl_glfb which names are eyes (it is a no-op consumer when the
    // null driver is doing the "rendering").
    kl_glfb_note_eye_texture(eye, stage, (uint32_t)handle);
    // P5: when the host has MTLTextures for the compositor to sample, the eye
    // texture's storage IS one of them — glEGLImageTargetTexture2DOES in place of
    // glTexStorage2D, and nothing else about this function changes (PLANNING
    // §12.9). The h,w transposition below applies identically, so it is passed on
    // in the same order.
    //
    // With no provider registered — every host run, so `make check` too — this is
    // a single NULL test and the GL path below is unchanged.
    if (kl_glfb_has_mtl_provider() &&
        kl_glfb_bind_eye_mtl_texture(eye, stage, (uint32_t)handle, h, w,
                                     KL_OVRP_TEXFMT_EYE))
        return 1;
    if (gl_BindTexture && gl_TexStorage2D) {
        gl_BindTexture(0x0DE1 /* GL_TEXTURE_2D */, (uint32_t)handle);
        // Allocate h-by-w, not w-by-h: the guest's own eye-resolve blit writes
        // a (0,0)-(h,w) region (measured: rb 2198x2304, blit rect 2198x2304,
        // args w=2304 h=2198), so the texture must be h wide and w tall or the
        // blit clips — losing a strip of the picture and leaving an
        // unwritten column of stale garbage at the right edge (the "narrow
        // vertical line" in the viewer). Whether the real signature orders
        // these h,w or Unity pre-transposes, the blit rect is ground truth.
        gl_TexStorage2D(0x0DE1, 1, KL_OVRP_TEXFMT_EYE, h, w);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// The layer family — Beat Saber 1.40's eye-texture seam
// ---------------------------------------------------------------------------
// 1.28 reached the eye textures through libunity's LEGACY VRDevice, which owns
// the GL names and hands them down to ovrp_SetupEyeTexture2 for storage (above).
// 1.40 is Unity 2022.3, where legacy VR is gone: libOculusXRPlugin.so is a real
// XR-SDK display provider and the ownership is the other way round. OVRPlugin —
// i.e. us — creates the eye textures and the provider ASKS for them:
//
//   OculusDisplayProvider::CreateLayer
//     ovrp_CalculateEyeLayerDesc3(layout, ..., &desc)   "what shape is an eye layer?"
//     ovrp_SetupLayer(device, &desc, &layerId)          "make me one"
//     ovrp_CalculateEyeViewportRect / ...PreviewRect    "where does each eye sit in it?"
//     ovrp_CalculateLayerDesc + ovrp_SetupLayer         ...again for a 1x1 dummy layer
//   OculusDisplayProvider::CreateEyeTextures
//     ovrp_GetLayerTextureStageCount(layerId, &n)
//     ovrp_GetLayerTexture2(layerId, stage, eye, &color, &depth)
//
// So ovrp_GetLayerTexture2 is 1.40's ovrp_SetupEyeTexture2, and it routes
// through the same two seams: kl_glfb_note_eye_texture (the capture's eye-FBO
// census) and kl_glfb_bind_eye_mtl_texture (P5's MTLTexture backing). Nothing
// downstream of those has to know which Unity version asked.
//
// Every ABI here is read off the REAL libOVRPlugin.so in this APK rather than
// inferred from the caller: each function's argument shuffle and NULL check say
// exactly which register carries the out-param, and every one of them ends in
// `cmp w0,#0 / csel w0,w0,wzr,lt` — plain ovrpResult, success 0. That matters
// twice over here, because the family is a chain: a positive return from any of
// them is the trap 10 shape and reads as the failure that killed GfxThread_Start
// (see g_ovrp_result_ok's note on ovrp_SetupDisplayObjects2).

// ovrpLayerDesc_EyeFov. The type NAME is the guest's own — libOculusXRPlugin
// exports `OculusSystem::SetRenderViewportScale(ovrpLayerDesc_EyeFov const&,
// ovrpEye, float, ovrpRecti*)` — and the OFFSETS are the ones the provider
// reads and writes in CreateLayer:
//
//   +0x08  read as an int and written back  -> TextureSize.w
//   +0x28 +0x2c                             -> Fov[0].LeftTan / .RightTan
//   +0x38 +0x3c                             -> Fov[1].LeftTan / .RightTan
//   +0x60  written with the adjusted width  -> MaxViewportSize.w
//
// which pins Fov[] at +0x20 with ovrpFovf = {Up, Down, Left, Right}, and so
// pins the whole base struct. The static asserts below are the guard: this
// layout is load-bearing for a struct the guest writes into as well as reads.
typedef struct { int w, h; } ovrp_sizei;
typedef struct { int x, y, w, h; } ovrp_recti;
typedef struct { float x, y, w, h; } ovrp_rectf;
typedef struct { float up, down, left, right; } ovrp_fovf;

typedef struct {
    int         shape;             // +0x00 ovrpShape
    int         layout;            // +0x04 ovrpLayout
    ovrp_sizei  texture_size;      // +0x08
    int         mip_levels;        // +0x10
    int         sample_count;      // +0x14
    int         format;            // +0x18 ovrpTextureFormat
    int         layer_flags;       // +0x1c
    ovrp_fovf   fov[2];            // +0x20
    ovrp_rectf  visible_rect[2];   // +0x40
    ovrp_sizei  max_viewport_size; // +0x60
    int         depth_format;      // +0x68
    int         mv_format;         // +0x6c
    int         mv_depth_format;   // +0x70
    ovrp_sizei  mv_texture_size;   // +0x74
} ovrp_layer_desc_eyefov;          //  = 0x7c

_Static_assert(offsetof(ovrp_layer_desc_eyefov, texture_size) == 0x08, "desc");
_Static_assert(offsetof(ovrp_layer_desc_eyefov, layer_flags) == 0x1c, "desc");
_Static_assert(offsetof(ovrp_layer_desc_eyefov, fov) == 0x20, "desc");
_Static_assert(offsetof(ovrp_layer_desc_eyefov, visible_rect) == 0x40, "desc");
_Static_assert(offsetof(ovrp_layer_desc_eyefov, max_viewport_size) == 0x60, "desc");

// ovrpTextureFormat -> GL internalformat.
//
// The enum's ORDER is the guest's own: libOculusXRPlugin.so carries the names as
// a string table (`ovrpTextureFormat_R8G8B8A8_sRGB`, `_R8G8B8A8`,
// `_R16G16B16A16_FP`, `_R11G11B10_FP`, `_B8G8R8A8_sRGB`, `_B8G8R8A8`, `_R5G6B5`,
// `_R16G16_FP`, `_A2B10G10R10`, `_D16`, `_D24_S8`, `_D32_FP`, `_D32_FP_S8`,
// `_None`), in that sequence, at 0x165194 onward. So the numbers are read out of
// the library rather than assumed, which matters because the two values this
// title passes — 0 and 10 — are meaningless without them.
//
// **This is what was black.** klovrp_GetLayerTexture2 allocated RGBA16F
// unconditionally, carried over from klovrp_SetupEyeTexture2 where 1.28's Unity
// really does render HDR. 1.40 asks for format 0, R8G8B8A8_sRGB, and its scene
// MSAA renderbuffer is GL_SRGB8_ALPHA8 (measured: `census fbo4: 5496000 lit —
// rb=11 fmt=0x8c43 2748x2880`) — so the guest's resolve blit was sRGB8 -> RGBA16F,
// which ES 3.0 makes INVALID_OPERATION. Unity reported it every frame
// ("OPENGL NATIVE PLUG-IN ERROR: GL_INVALID_OPERATION") and the eye texture
// stayed at 0 lit while the scene behind it was fully drawn.
static uint32_t klovrp_gl_format(int ovrp_fmt, const char **name) {
    switch (ovrp_fmt) {
    case 0:  if (name) *name = "R8G8B8A8_sRGB";    return 0x8C43;  // GL_SRGB8_ALPHA8
    case 1:  if (name) *name = "R8G8B8A8";         return 0x8058;  // GL_RGBA8
    case 2:  if (name) *name = "R16G16B16A16_FP";  return 0x881A;  // GL_RGBA16F
    case 3:  if (name) *name = "R11G11B10_FP";     return 0x8C3A;  // GL_R11F_G11F_B10F
    // B8G8R8A8 has no ES internalformat — GL orders channels by the *format*
    // argument, not the internalformat, and there is no GL_BGRA8 to allocate.
    // RGBA8 is the same storage; a guest that means BGRA has to say so at upload
    // time, and nothing here uploads.
    case 4:  if (name) *name = "B8G8R8A8_sRGB";    return 0x8C43;
    case 5:  if (name) *name = "B8G8R8A8";         return 0x8058;
    case 6:  if (name) *name = "R5G6B5";           return 0x8D62;  // GL_RGB565
    case 7:  if (name) *name = "R16G16_FP";        return 0x822F;  // GL_RG16F
    case 8:  if (name) *name = "A2B10G10R10";      return 0x8059;  // GL_RGB10_A2
    default: if (name) *name = NULL;               return 0;
    }
}

// ovrpShape_EyeFov, and ovrpLayout's {Stereo, Mono, DoubleWide, Array}. The
// provider derives its layout argument from Unity's texture-layout choice and
// passes only 0 (separate 2D textures per eye) or 3 (one 2D array, two slices);
// this title asks for 0, measured.
#define KLOVRP_SHAPE_EYEFOV   3
#define KLOVRP_LAYOUT_STEREO  0
#define KLOVRP_LAYOUT_ARRAY   3

// One entry per layer the provider sets up: the eye layer, and the 1x1 dummy
// layer it makes afterwards on GLES. Four slots for two layers, because a
// display-subsystem restart destroys and recreates both.
#define KLOVRP_MAX_LAYERS 4
static struct klovrp_layer {
    int      id;                                  // 0 = free; ids start at 1
    ovrp_layer_desc_eyefov desc;                  // as ovrp_SetupLayer received it
    uint32_t tex[KLOVRP_MAX_STAGES][2];           // GL names, per stage per eye
    int      is_eye;                              // eye layer, or the dummy
    int      used;                                // this slot has been set up before
} g_layers[KLOVRP_MAX_LAYERS];
static int g_next_layer_id = 1;
// The size the eye storage currently IS, as opposed to what any layer's desc
// says it should be. There is one set of eye textures for the whole process —
// kl_glfb keys them by (eye, stage) — so this is a property of the runtime and
// not of a layer, which is exactly why a per-layer desc could not express it.
// 0 until the first eye texture is handed out. See klovrp_SetupLayer.
static int g_eye_storage_w, g_eye_storage_h;

static struct klovrp_layer *klovrp_layer(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < KLOVRP_MAX_LAYERS; i++)
        if (g_layers[i].id == id) return &g_layers[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// ovrpLayerSubmit — what ovrp_EndFrame4 is handed, and where the render
// viewport is stated
// ---------------------------------------------------------------------------
// **This layout is not inferred.** libOculusXRPlugin.so in this APK ships with
// DWARF, so the struct is read out of the guest's own debug info rather than
// guessed from a caller's stores:
//
//     0x00 int          LayerId
//     0x04 int          TextureStage
//     0x08 ovrpRecti    ViewportRect[2]        <-- what this exists for
//     0x28 ovrpPosef    Pose
//     0x44 int          LayerSubmitFlags
//     0x48 ovrpVector4f ColorScale
//     0x58 ovrpVector4f ColorOffset
//     0x68 ovrpBool     OverrideTextureRectMatrix
//     0x6c ovrpTextureRectMatrixf TextureRectMatrix
//     ...                                       (0xbc total)
//
// and ovrpRecti is {ovrpVector2i Pos; ovrpSizei Size;}, four ints, 0x10 bytes.
// `OculusDisplayProvider::SubmitFrame` then confirms it from the other side:
// it stores the two rects at submit+0x08 and submit+0x18, the identity Pose at
// +0x28, and the flags at +0x44, which pins every offset above twice over.
//
// **Where the rect comes from, and why it is the resolution knob.** In
// `OculusDisplayProvider::CreateLayer` the provider takes Unity's
// `renderViewportScale` out of the UnityXRFrameSetupHints (+0xc) and calls
// `OculusSystem::SetRenderViewportScale(desc, eye, scale, &rect)` once per eye
// — which is nothing but a forwarding wrapper around our own
// ovrp_CalculateEyeViewportRect, whose answer is `{0, 0, w*scale, h*scale}`.
// It caches both rects and submits them here, every frame. So the guest asks
// US where to render, renders exactly there, tells us it did — and until this
// was parsed, all three of those succeeded and the compositor still sampled
// the whole texture.
//
// Overlay layers (a second submit) are ignored on purpose: only the eye layer
// describes the picture the compositor reprojects, and it is identified by the
// id we handed out at ovrp_SetupLayer rather than by position in the list.
typedef struct {
    int        layer_id;      // +0x00
    int        texture_stage; // +0x04
    ovrp_recti viewport[2];   // +0x08
    // The rest is read by nobody here; it is named so the size assert below is
    // a real check on the layout rather than on a prefix of it.
    float      pose[7];       // +0x28 ovrpPosef {Orientation xyzw, Position xyz}
    int        submit_flags;  // +0x44
    float      color_scale[4];   // +0x48
    float      color_offset[4];  // +0x58
    int        override_texture_rect_matrix; // +0x68
    float      texture_rect_matrix[16];      // +0x6c
    int        override_per_layer_color;     // +0xac
    int        has_blend_factors;            // +0xb0
    int        src_blend_factor;             // +0xb4
    int        dst_blend_factor;             // +0xb8
} ovrp_layer_submit;                         //  = 0xbc

_Static_assert(offsetof(ovrp_layer_submit, viewport) == 0x08, "submit");
_Static_assert(offsetof(ovrp_layer_submit, pose) == 0x28, "submit");
_Static_assert(offsetof(ovrp_layer_submit, submit_flags) == 0x44, "submit");
_Static_assert(offsetof(ovrp_layer_submit, override_texture_rect_matrix) == 0x68, "submit");
_Static_assert(offsetof(ovrp_layer_submit, dst_blend_factor) == 0xb8, "submit");
_Static_assert(sizeof(ovrp_layer_submit) == 0xbc, "submit");
_Static_assert(sizeof(ovrp_recti) == 0x10, "recti");

// See the forward declaration beside klovrp_end_frame_impl.
//
// The argument is an array of POINTERS to submits (`const ovrpLayerSubmit* const*`),
// which is the one part of this a struct layout cannot tell you and the
// signature can.
// `of` takes the layer's texture size — the size the rects are relative to.
// A rect in pixels means nothing without it (kl_ovrp.h, viewport_of).
// The eye layer's texture stage, straight out of the submit. Only the EYE layer
// is consulted — an overlay submit carries its own unrelated stage, and picking
// the wrong one would read back a texture the guest never drew this frame.
// -1 for "the guest named no eye layer", which is NOT the same as stage 0 — a
// caller that cannot tell them apart files every unnamed frame against stage 0
// and the picture is one stage stale forever. Both callers make the distinction
// explicitly.
// What a non-eye submit carries PAST the common header, which is the one thing
// a compositor for overlay layers still needs and nothing here has ever read.
//
// The array the guest hands ovrp_EndFrame4 is of `ovrpLayerSubmit*`, but the
// objects behind those pointers are `ovrpLayerSubmitUnion` — the real plugin's
// own signature says so: `CompositorVRAPI::EndFrame(int, std::vector<
// ovrpLayerSubmitUnion>&, bool, void*)`. And the union's Quad/Cylinder arms add
// an `ovrpVector3f` after the header, which is the layer's WORLD SIZE in
// metres: `Compositor::EnqueueSubmitLayer(..., ovrpPosef, ovrpVector3f, ...)`
// passes exactly that pair, and `CompositorVRAPI::calculateTexCoordsMatrix(
// const ovrpPosef&, ..., const ovrpVector3f&, ...)` consumes it.
//
// The OFFSET is read out of `Compositor::ImportLayerSubmit(const
// ovrpLayerSubmit*, ovrpLayerSubmitUnion*)` (0x3c9cc in RE4's copy) rather than
// guessed, and it is 0xb0, not the 0xbc our header struct ends at:
//
//   shape 0 (Quad)     ldr x8,[x20+0xb0] ; str x8,[x19+0xb0]   — EIGHT bytes
//   shape 1 (Cylinder) three words at +0xb0, +0xb4, +0xb8
//   shape 3 (EyeFov)   its own block from +0xf0 on
//
// so the common header this plugin was built with ends at 0xb0 and everything
// past it is the union's per-shape arm. (Each of those has an alternate source
// offset — +0x48, +0x4c, +0x50 — selected by a flag: that is the LEGACY submit
// whose header stopped at 0x44, before ColorScale existed. Our struct's
// `has_blend_factors`/`src`/`dst` names at 0xb0..0xbc come from libOculusXRPlugin's
// DWARF, i.e. from a NEWER OVRPlugin, and nothing has ever read them — they are
// the Cylinder arm here.)
//
// `sizeof(ovrpLayerSubmitUnion)` is 0x130, from ovrp_EndFrame4's own
// `mov w8,#0x130 / mul` when it allocates the vector — so reading +0xb0 on a
// submit the guest built as a union is in bounds. Printed rather than acted on:
// nothing composites overlay layers yet, and a pose without a size cannot place
// a quad, so this is what the next step is built from.
static void klovrp_probe_submit_tail(const ovrp_layer_submit *s) {
    const float *q = (const float *)((const unsigned char *)s + 0xb0);
    fprintf(stderr, "          union +0xb0: %.4f %.4f  "
                    "(ovrpLayerSubmitQuad.Size, metres)\n", q[0], q[1]);
}

// ovrpShape, from the guest's own enum. The names are in libOculusXRPlugin's
// string table in this order; UE4's OculusHMD uses Quad for splash screens and
// for IStereoLayers, Cylinder and Equirect for the rest.
static const char *klovrp_shape_name(int shape) {
    static const char *const N[] = { "Quad", "Cylinder", "Cubemap", "EyeFov",
                                     "OffcenterCubemap", "Equirect", "ReconstructionPassthrough",
                                     "SurfaceProjectedPassthrough", "Fisheye",
                                     "KeyboardHandsPassthrough", "KeyboardMaskedHandsPassthrough" };
    return (unsigned)shape < sizeof N / sizeof *N ? N[shape] : "?";
}

// The whole submit list, which nothing here has ever printed.
//
// Every other reader of this array walks past anything that is not the eye
// layer — deliberately, because only the eye layer describes the picture the
// compositor reprojects. The consequence is that a guest submitting OVERLAY
// layers looks, from every log this project produces, exactly like one that
// submits none: RE4 submits its intro logos that way and the run said nothing
// at all about them.
//
// So this is a census rather than a per-frame trace: one line per distinct
// (layer, shape, stage, viewport, pose, flags) combination, printed when it
// first appears and when it changes. A title that submits one eye layer forever
// pays one line for the run.
static void klovrp_census_submits(const void *layer_submits, int count) {
    if (!kl_env_on("KL_OVRP_LAYERS", 0)) return;
    const ovrp_layer_submit *const *list = layer_submits;
    if (!list || count <= 0) return;
    // Keyed on the layer id, so an overlay that moves says so and one that is
    // static says it once.
    static struct { int id, live; ovrp_layer_submit last; } seen[KLOVRP_MAX_LAYERS * 2];
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        int slot = -1;
        for (unsigned k = 0; k < sizeof seen / sizeof *seen; k++) {
            if (seen[k].live && seen[k].id == s->layer_id) { slot = (int)k; break; }
            if (!seen[k].live && slot < 0) slot = (int)k;
        }
        if (slot < 0) continue;
        int fresh = !seen[slot].live || seen[slot].id != s->layer_id;
        if (!fresh && memcmp(&seen[slot].last, s, sizeof *s) == 0) continue;
        seen[slot].live = 1;
        seen[slot].id = s->layer_id;
        seen[slot].last = *s;

        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        fprintf(stderr, "  [ovrp] submit %d/%d: layer %d (%s%s) stage %d "
                        "vp0 %d,%d %dx%d vp1 %d,%d %dx%d flags=%#x\n",
                i + 1, count, s->layer_id,
                l ? klovrp_shape_name(l->desc.shape) : "unknown layer",
                l && l->is_eye ? ", EYE" : "",
                s->texture_stage,
                s->viewport[0].x, s->viewport[0].y, s->viewport[0].w, s->viewport[0].h,
                s->viewport[1].x, s->viewport[1].y, s->viewport[1].w, s->viewport[1].h,
                (unsigned)s->submit_flags);
        // The pose is what says whether an overlay is head-locked, world-locked
        // or identity — the difference the user of a composite actually sees, and
        // the one thing no other line here carries.
        fprintf(stderr, "          pose q=(%.3f %.3f %.3f %.3f) p=(%.3f %.3f %.3f)%s"
                        "  size %dx%d\n",
                s->pose[0], s->pose[1], s->pose[2], s->pose[3],
                s->pose[4], s->pose[5], s->pose[6],
                (s->pose[0] == 0 && s->pose[1] == 0 && s->pose[2] == 0 &&
                 s->pose[3] == 1 && s->pose[4] == 0 && s->pose[5] == 0 &&
                 s->pose[6] == 0) ? "  [identity]" : "",
                l ? l->desc.texture_size.w : 0, l ? l->desc.texture_size.h : 0);
        if (l && !l->is_eye) klovrp_probe_submit_tail(s);
    }
}

// The non-eye layers of the current frame — see kl_ovrp.h.
//
// Replaced whole under g_frames.mu at each ovrp_EndFrame4, so a compositor
// reading it concurrently sees the previous frame's list rather than half of
// two. Sized like g_layers, because a submit names a layer and there cannot be
// more of those than exist.
static struct {
    kl_ovrp_overlay v[KLOVRP_MAX_LAYERS];
    int             n;
} g_overlays;

// ovrpLayerSubmitFlag_HeadLocked. The value is the guest's own: this is bit 1
// in every OVRPlugin the corpus carries (bit 0 is NoDepth), and RE4's splash
// submits flags = 0, i.e. WORLD-locked, which is the case the composite
// implements. A head-locked layer is reported by name rather than placed with
// the world-locked math, because the two differ by the whole head pose and a
// wrong placement has no error surface at all.
#define KLOVRP_SUBMIT_HEAD_LOCKED 0x2

static void klovrp_record_overlays(const void *layer_submits, int count) {
    const ovrp_layer_submit *const *list = layer_submits;
    kl_ovrp_overlay tmp[KLOVRP_MAX_LAYERS];
    int n = 0;
    for (int i = 0; list && i < count && n < KLOVRP_MAX_LAYERS; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l || l->is_eye) continue;
        kl_ovrp_overlay *o = &tmp[n++];
        memset(o, 0, sizeof *o);
        o->layer_id = s->layer_id;
        o->shape    = l->desc.shape;
        o->stage    = s->texture_stage;
        o->tex_w    = l->desc.texture_size.w;
        o->tex_h    = l->desc.texture_size.h;
        for (int e = 0; e < 2; e++) {
            o->viewport[e][0] = s->viewport[e].x;
            o->viewport[e][1] = s->viewport[e].y;
            o->viewport[e][2] = s->viewport[e].w;
            o->viewport[e][3] = s->viewport[e].h;
        }
        memcpy(o->pose, s->pose, sizeof o->pose);
        o->flags       = s->submit_flags;
        o->head_locked = (s->submit_flags & KLOVRP_SUBMIT_HEAD_LOCKED) != 0;
        // The quad's world size, out of the union's own arm. The OFFSET is read
        // from Compositor::ImportLayerSubmit rather than from our struct's end
        // — see klovrp_probe_submit_tail, which is where that measurement lives.
        if (o->shape == 0) {
            const float *q = (const float *)((const unsigned char *)s + 0xb0);
            o->size[0] = q[0];
            o->size[1] = q[1];
        }
    }
    pthread_mutex_lock(&g_frames.mu);
    memcpy(g_overlays.v, tmp, sizeof tmp);
    g_overlays.n = n;
    pthread_mutex_unlock(&g_frames.mu);
}

int kl_ovrp_overlay_count(void) {
    pthread_mutex_lock(&g_frames.mu);
    int n = g_overlays.n;
    pthread_mutex_unlock(&g_frames.mu);
    return n;
}

int kl_ovrp_overlay_get(int i, kl_ovrp_overlay *out) {
    if (!out || i < 0) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int ok = i < g_overlays.n;
    if (ok) *out = g_overlays.v[i];
    pthread_mutex_unlock(&g_frames.mu);
    return ok;
}

static int klovrp_submit_stage(const void *layer_submits, int count) {
    const ovrp_layer_submit *const *list = layer_submits;
    if (!list || count <= 0) return -1;
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (l && l->is_eye) return s->texture_stage;
    }
    return -1;
}

static int klovrp_submit_viewports(const void *layer_submits, int count, int vp[8],
                                   int of[2]) {
    const ovrp_layer_submit *const *list = layer_submits;
    if (!list || count <= 0) return 0;
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l || !l->is_eye) continue;
        for (int eye = 0; eye < 2; eye++) {
            vp[eye * 4 + 0] = s->viewport[eye].x;
            vp[eye * 4 + 1] = s->viewport[eye].y;
            vp[eye * 4 + 2] = s->viewport[eye].w;
            vp[eye * 4 + 3] = s->viewport[eye].h;
        }
        of[0] = l->desc.texture_size.w;
        of[1] = l->desc.texture_size.h;
        // Said when it CHANGES, which is the event, and said for both eyes
        // because a per-eye difference is the one thing a single unwarp grid
        // cannot express (see the compositors). Silent on a run that never
        // scales, so this costs a healthy title nothing.
        static int have_last;
        static int last[8];
        if (!have_last || memcmp(last, vp, sizeof last) != 0) {
            have_last = 1;
            memcpy(last, vp, sizeof last);
            fprintf(stderr, "  [ovrp] render viewport: eye0 %d,%d %dx%d  eye1 %d,%d %dx%d "
                            "of a %dx%d eye texture (%.1f%% of the width)\n",
                    vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7],
                    l->desc.texture_size.w, l->desc.texture_size.h,
                    l->desc.texture_size.w > 0
                        ? 100.0 * (double)vp[2] / (double)l->desc.texture_size.w : 0.0);
        }
        return 1;
    }
    return 0;
}

// Fill a desc for an eye layer. The arguments ARE the answer for the format and
// count fields — this entry point's whole job is "turn these parameters into the
// desc that describes them" — and the geometry is ours: the per-eye render
// target size the display seam measured (kl_ovrp_eye_texture_size) and the
// frustum tangents it measured with it (kl_ovrp_set_eye_frustum).
//
// textureScale is the render-scale multiplier Unity carries in its frame setup
// hints; the real plugin applies it to the recommended size, so we do too, and
// clamp to at least 1 pixel so a hint of 0 cannot produce a zero-sized layer.
static void klovrp_fill_eye_desc(ovrp_layer_desc_eyefov *d, int layout,
                                 int mip_levels, int sample_count, int format,
                                 int depth_format, int mv_format,
                                 int mv_depth_format,
                                 int layer_flags, float texture_scale) {
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    if (!(texture_scale > 0.0f)) texture_scale = 1.0f;
    w = (int)((float)w * texture_scale);
    h = (int)((float)h * texture_scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    memset(d, 0, sizeof *d);
    d->shape        = KLOVRP_SHAPE_EYEFOV;
    d->layout       = layout;
    d->texture_size = (ovrp_sizei){ w, h };
    d->mip_levels   = mip_levels > 0 ? mip_levels : 1;
    d->sample_count = sample_count > 0 ? sample_count : 1;
    // Echoed, and then HONOURED: klovrp_GetLayerTexture2 allocates the storage
    // through klovrp_gl_format(d->format), so the number reported and the
    // storage made cannot disagree. That is the whole fix for the black eye
    // texture — see klovrp_gl_format.
    d->format       = format;
    d->layer_flags  = layer_flags;
    for (int eye = 0; eye < 2; eye++) {
        // g_eye_tan is {left, right, top, bottom}; ovrpFovf is {up, down, left,
        // right}. Transposing these is a silently wrong frustum, so it happens
        // in exactly one place.
        const float *et = klovrp_eye_tan(eye);
        d->fov[eye] = (ovrp_fovf){ et[2], et[3], et[0], et[1] };
        d->visible_rect[eye] = (ovrp_rectf){ 0.0f, 0.0f, 1.0f, 1.0f };
    }
    d->max_viewport_size = d->texture_size;
    // The remaining format fields are echoed for the same reason: they are what
    // the caller asked for. No depth or motion-vector TEXTURE is handed out —
    // ovrp_GetLayerTexture2 answers 0 for the depth id and nothing here submits
    // motion vectors — so these describe the formats a provider would use if it
    // asked, and it does not.
    d->depth_format    = depth_format;
    d->mv_format       = mv_format;
    d->mv_depth_format = mv_depth_format;
}

// ovrp_CalculateEyeLayerDesc3(layout, mipLevels, sampleCount, format,
//                             depthFormat, mvFormat, mvDepthFormat, layerFlags,
//                             desc*, textureScale, scale2)
//
// Eight integer args in x0..x7, the out pointer as the ninth (the real 0x16e5f0
// reads it from [x29+0x30], i.e. the first stack slot, and -1001s on NULL), and
// two floats in s0/s1.
//
// The four format arguments are placed by lining the three numbered forms up
// against each other in the real library: each one shuffles its integer args
// down by one and calls a common builder, and the un-suffixed 0x179bc0 fills the
// builder's last three format slots with 10 (D24_S8), ...2 (0x16e510) fills the
// last two, and ...3 passes all three. So the argument this title sets to 0 is
// the COLOUR format and the ones after it are depth and motion-vector — which is
// then confirmed from the other side: format 0 is R8G8B8A8_sRGB and Unity's
// scene MSAA renderbuffer really is GL_SRGB8_ALPHA8.
static uint64_t klovrp_CalculateEyeLayerDesc3(int layout, int mip_levels,
                                              int sample_count, int format,
                                              int depth_format, int mv_format,
                                              int mv_depth_format, int layer_flags,
                                              ovrp_layer_desc_eyefov *out,
                                              float texture_scale, float scale2) {
    ovrp_hit("ovrp_CalculateEyeLayerDesc3");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    klovrp_fill_eye_desc(out, layout, mip_levels, sample_count, format,
                         depth_format, mv_format, mv_depth_format,
                         layer_flags, texture_scale);
    const char *fname = NULL;
    uint32_t gl = klovrp_gl_format(format, &fname);
    fprintf(stderr, "  [ovrp] CalculateEyeLayerDesc: layout=%d %dx%d mips=%d "
                    "samples=%d fmt=%d (%s -> GL %#x) depth=%d mv=%d/%d "
                    "flags=%#x scale=%.3f/%.3f\n",
            layout, out->texture_size.w, out->texture_size.h, mip_levels,
            sample_count, format, fname ? fname : "UNMAPPED", gl,
            depth_format, mv_format, mv_depth_format, (unsigned)layer_flags,
            (double)texture_scale, (double)scale2);
    return OVRP_SUCCESS;
}

// The ...2 form: one float, no motion-vector formats, and the out pointer in x6
// (the real 0x16e510 -1001s on a NULL x6, and fills the builder's last two
// format slots with 10 itself). Same answer — this is an ABI revision, not a
// different question.
static uint64_t klovrp_CalculateEyeLayerDesc2(int layout, int mip_levels,
                                              int sample_count, int format,
                                              int depth_format, int layer_flags,
                                              ovrp_layer_desc_eyefov *out,
                                              float texture_scale) {
    ovrp_hit("ovrp_CalculateEyeLayerDesc2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    klovrp_fill_eye_desc(out, layout, mip_levels, sample_count, format,
                         depth_format, 10 /* D24_S8, as ...2 itself does */,
                         10, layer_flags, texture_scale);
    return OVRP_SUCCESS;
}

// ovrp_CalculateLayerDesc(shape, layout, textureSize, mipLevels, sampleCount,
//                         format, layerFlags, desc*)
//
// The non-eye sibling: every layer that is not the eye layer goes through it —
// Unity's 1x1 dummy, and an Unreal guest's SPLASH and stereo-layer quads, which
// is where its size stopped being ignorable.
//
// **textureSize is a POINTER, and this answered 1x1 for the whole project.**
// The old note here read the guest's address argument as the guest's bug ("its
// header and the shipped library disagree about by-value vs by-pointer") and
// answered a size we invented. It is the other way round, and the library says
// so twice over. The mangled name of the function this forwards to is
// `...CalculateLayerDescE9ovrpShape10ovrpLayout RK9ovrpSizei ii...` — `RK` is
// `const&`. And its body (0x3ccdc in RE4's copy) dereferences it three times:
// `ldr w9,[x21]` for the width, `ldr w10,[x21,#4]` for the height, and then
// `ldr x8,[x21] / str x8,[x26,#8]` copying both into desc.TextureSize.
//
// Trap 10b's family, argument half — and unlike the others in it, the wrong
// answer was not a stack slot left unwritten but a SIZE, which the guest then
// built a layer out of. For Unity it cost nothing visible: its only non-eye
// layer really is a 1x1 it never renders into. For RE4 it is the intro logos —
// a splash quad told it was 1x1.
//
// mipLevels 0 means "the full chain", and the real body computes it the same
// way for both axes and takes the smaller: floor(log2(n)) + 1.
//
// What it does NOT fill is as measured as what it does: the memset covers +0x08
// onward and only shape/layout/size/mips/samples/format/flags are written
// after, so VisibleRect and MaxViewportSize stay ZERO. Filling them with
// something reasonable would be inventing an answer the guest can tell apart
// from a real plugin's.
static int klovrp_mip_chain(int n) {
    int c = 1;
    if (n >= 2) do { c++; } while ((n >>= 1) > 3);
    return c;
}

static uint64_t klovrp_CalculateLayerDesc(int shape, int layout,
                                          const ovrp_sizei *texture_size,
                                          int mip_levels, int sample_count, int format,
                                          int layer_flags,
                                          ovrp_layer_desc_eyefov *out) {
    ovrp_hit("ovrp_CalculateLayerDesc");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    // The real body reads the pointer with no NULL check of its own, so a NULL
    // here would be a fault inside the plugin. Refusing is the one deviation,
    // because a fault in our address space is reported as ours.
    if (!texture_size) return OVRP_FAIL_INVALID_PARAM;
    ovrp_sizei sz = *texture_size;
    memset(out, 0, sizeof *out);
    out->shape        = shape;
    out->layout       = layout;
    out->texture_size = sz;
    out->mip_levels   = mip_levels > 0 ? mip_levels
                      : (klovrp_mip_chain(sz.w) < klovrp_mip_chain(sz.h)
                         ? klovrp_mip_chain(sz.w) : klovrp_mip_chain(sz.h));
    out->sample_count = sample_count;
    out->format       = format;
    out->layer_flags  = layer_flags;
    fprintf(stderr, "  [ovrp] CalculateLayerDesc: shape=%d layout=%d %dx%d "
                    "mips=%d samples=%d fmt=%d flags=%#x\n",
            shape, layout, sz.w, sz.h, out->mip_levels, sample_count,
            format, (unsigned)layer_flags);
    return OVRP_SUCCESS;
}

// ovrp_SetupLayer(void* device, const ovrpLayerDesc* desc, int* layerId).
// The real 0x16df60 NULL-checks the THIRD argument, which is what identifies
// x2 as the out-param rather than the device.
static uint64_t klovrp_SetupLayer(void *device, const ovrp_layer_desc_eyefov *desc,
                                  int *layer_id) {
    ovrp_hit("ovrp_SetupLayer");
    if (!layer_id || !desc) return OVRP_FAIL_INVALID_PARAM;
    // An eye layer is the one carrying a frustum. The dummy layer's desc has no
    // Fov at all (klovrp_CalculateLayerDesc zeroes it), so this is the guest's
    // own distinction rather than a call-order guess.
    int is_eye = desc->fov[0].left > 0.0f || desc->fov[0].right > 0.0f;

    // Prefer a freed slot that describes the SAME layer, and keep its textures.
    // GfxThread_Stop destroys both layers and GfxThread_Start makes them again —
    // it happened on the very first 1.40 run — so a fresh slot per setup both
    // exhausts the table on the third cycle and hands the compositor a new set of
    // GL names for a picture that has not changed. Matching on the geometry is
    // what makes the reuse safe: a layer of a different size needs new storage.
    //
    // **A slot's textures are not its own, and matching the geometry was not
    // enough.** kl_glfb keys eye textures by (eye, stage) and nothing else, so
    // an eye layer of a DIFFERENT size taking those slots overwrites them — and
    // the freed slot goes on claiming storage it no longer has. 1.40 alternates
    // two eye descs (3072x2464 at textureScale 1.0 and 3686x2956 at 1.2), so on
    // device this happened four times in a ten-second run:
    //
    //   SetupLayer -> layer 6, EYE, 3072x2464 (reusing the previous layer's textures)
    //   [cp] unwarp grid ... for a 3686x2956 eye texture (viewport 0,0 3072x2464)
    //
    // — the guest holding a 3072x2464 layer whose textures are 3686x2956, which
    // then makes every size-derived thing on the path disagree at once: the eye
    // texture the compositor samples, the rect the guest submits, and (with
    // KL_VRR on) whether the guest's MSAA target matches the rate map at all,
    // which kl_glfb reports as "left unfoveated" in the same run.
    //
    // So the reuse now also requires that the storage still BE that size.
    // A same-size restart — the case this exists for — is unaffected.
    struct klovrp_layer *l = NULL;
    for (int i = 0; i < KLOVRP_MAX_LAYERS && !l; i++) {
        struct klovrp_layer *c = &g_layers[i];
        if (c->id || !c->used) continue;
        if (c->is_eye == is_eye &&
            c->desc.texture_size.w == desc->texture_size.w &&
            c->desc.texture_size.h == desc->texture_size.h &&
            c->desc.layout == desc->layout &&
            (!is_eye || !g_eye_storage_w ||
             (g_eye_storage_w == desc->texture_size.w &&
              g_eye_storage_h == desc->texture_size.h)))
            l = c;
    }
    if (!l && is_eye && g_eye_storage_w &&
        (g_eye_storage_w != desc->texture_size.w ||
         g_eye_storage_h != desc->texture_size.h))
        fprintf(stderr, "  [ovrp] SetupLayer: a freed %dx%d eye slot exists, but the "
                        "eye storage is %dx%d now — allocating fresh rather than "
                        "handing back textures another layer took\n",
                desc->texture_size.w, desc->texture_size.h,
                g_eye_storage_w, g_eye_storage_h);
    int reused = l != NULL;
    for (int i = 0; i < KLOVRP_MAX_LAYERS && !l; i++)
        if (!g_layers[i].id) { l = &g_layers[i]; memset(l, 0, sizeof *l); }
    if (!l) {
        fprintf(stderr, "  [ovrp] SetupLayer: all %d layer slots are live — this "
                        "guest creates two (an eye layer and a 1x1 dummy)\n",
                KLOVRP_MAX_LAYERS);
        return OVRP_FAIL_INVALID_PARAM;
    }
    // Ids start at 1 and never repeat: 0 is what an out-slot the guest zeroed
    // already holds, so an id indistinguishable from "never set" is a class of
    // bug we can decline to have, and a reused id would make a stale handle from
    // before the restart address the new layer.
    l->id     = g_next_layer_id++;
    l->desc   = *desc;
    l->is_eye = is_eye;
    l->used   = 1;
    *layer_id = l->id;
    // Named by its SHAPE rather than as "dummy". Unity's one non-eye layer
    // really is a placeholder, and calling every other guest's overlays that
    // read as a spare: RE4's splash is a 900x900 Quad and the line said
    // "dummy, 1x1" (the size because ovrp_CalculateLayerDesc answered one it
    // invented — see there).
    fprintf(stderr, "  [ovrp] SetupLayer(device=%p) -> layer %d, %s, %dx%d "
                    "layout=%d fmt=%d samples=%d%s\n",
            device, l->id, is_eye ? "EYE" : klovrp_shape_name(desc->shape),
            desc->texture_size.w, desc->texture_size.h, desc->layout,
            desc->format, desc->sample_count,
            reused ? " (reusing the previous layer's textures)" : "");
    return OVRP_SUCCESS;
}

static uint64_t klovrp_DestroyLayer(int layer_id) {
    ovrp_hit("ovrp_DestroyLayer");
    struct klovrp_layer *l = klovrp_layer(layer_id);
    // The teardown mirror. The GL names are NOT deleted here, and the slot keeps
    // them: this arrives on the provider's gfx-thread stop, the eye textures are
    // still attached to framebuffers the capture and the compositor hold, and
    // deleting a name the compositor is sampling is a worse failure than holding
    // it across a restart that is about to ask for the same layer again.
    // klovrp_SetupLayer above is the other half — it hands the same textures back.
    if (l) l->id = 0;
    return OVRP_SUCCESS;
}

// ovrp_GetLayerTextureStageCount(layerId, int* out) — real 0x16e160,
// -1001 on a NULL second argument.
static uint64_t klovrp_GetLayerTextureStageCount(int layer_id, int *out) {
    ovrp_hit("ovrp_GetLayerTextureStageCount");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    struct klovrp_layer *l = klovrp_layer(layer_id);
    // The eye swapchain's depth is the one the rest of kl_ovrp is built around
    // (KL_OVRP_STAGES, the frame ring, the compositor's stage association). The
    // dummy layer needs exactly one — it is never rendered into.
    *out = (l && l->is_eye) ? kl_ovrp_stage_count() : 1;
    return OVRP_SUCCESS;
}

// ovrp_GetLayerTexture2(layerId, stage, eye, uint64_t* color, uint64_t* depth).
// The real 0x16e1c0 requires at least one of the two out pointers (`orr x8, x3,
// x5 / cbz x8 -> -1001`), so each is optional on its own.
//
// **The out-params are 64 bits wide, not 32.** The guest reads the slot back
// with a full `ldr x8, [sp+0x78]` (CreateTexture+0x1b0) and the slot is OUTSIDE
// the range it memset at entry, so writing only the low half leaves the top half
// holding whatever was on the provider's stack — a GL name with garbage in bits
// 32..63, handed to Unity as its native texture. A 64-bit slot for what is a
// GLuint here is how OVRPlugin carries a VkImage in the same field.
//
// This is 1.40's eye-texture creation point, and the counterpart of 1.28's
// ovrp_SetupEyeTexture2: the name is OURS to make, where in 1.28 Unity made it
// and handed it down. Everything after that is the same seam.
static uint64_t klovrp_GetLayerTexture2(int layer_id, int stage, int eye,
                                        uint64_t *color, uint64_t *depth) {
    ovrp_hit("ovrp_GetLayerTexture2");
    if (!color && !depth) return OVRP_FAIL_INVALID_PARAM;
    struct klovrp_layer *l = klovrp_layer(layer_id);
    if (!l) return OVRP_FAIL_INVALID_PARAM;
    if ((unsigned)stage >= KLOVRP_MAX_STAGES || (unsigned)eye > 1)
        return OVRP_FAIL_INVALID_PARAM;
    // One texture per (stage, eye) is the Stereo layout. Under ovrpLayout_Array
    // the two eyes are SLICES of one array texture, and handing back two
    // separate 2D names there would be accepted, wired up, and render to the
    // wrong storage with no error anywhere.
    //
    // The Array layout is served on the VULKAN path only (the branch below
    // makes a two-layer VkImage and gives both eyes the same handle) and
    // refused by name everywhere else, because the GL gateway has no multiview
    // to render it with. A guest only reaches here asking for Array if
    // kl_ovrp_multiview() said yes, so a refusal here would be the two answers
    // disagreeing — it is kept as the guard for exactly that.
    if (l->is_eye && l->desc.layout == KLOVRP_LAYOUT_ARRAY &&
        kl_vulkan_guest_active() && kl_ovrp_multiview()) {
        /* served below */
    } else if (l->is_eye && l->desc.layout != KLOVRP_LAYOUT_STEREO) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] GetLayerTexture2: layer %d asks for layout %d "
                            "(%s); only %d (Stereo, one texture per eye) is "
                            "implemented — refusing rather than binding the wrong "
                            "storage\n", layer_id, l->desc.layout,
                    l->desc.layout == KLOVRP_LAYOUT_ARRAY ? "Array" : "?",
                    KLOVRP_LAYOUT_STEREO);
        }
        return OVRP_FAIL_UNSUPPORTED;
    }
    // No depth storage is provided. Answering 0 is the truthful "this layer has
    // no depth texture" — the provider asks for both in one call and uses
    // whichever it got.
    if (depth) *depth = 0;
    if (!color) return OVRP_SUCCESS;

    // BONELAB / the Vulkan path. Everything below this branch makes a GL texture
    // NAME, and on a Vulkan guest that name is read back as a VkImage handle and
    // handed to vkCreateImageView — which is a segfault inside MoltenVK with a
    // small integer for an address (measured: `MVKImageView::MVKImageView+0x90`,
    // fault at 0x9, from libunity's own CreateTexture). The 64-bit out-parameter
    // this function has always filled is exactly the field OVRPlugin carries a
    // VkImage in, so nothing about the seam changes except what makes the
    // storage.
    //
    // The test is what the guest DID — whether it brought a Vulkan device up
    // through kl_vulkan.c — rather than a renderer enum we were told. A guest
    // that resolves vk* and then picks GLES answers false here, correctly.
    if (kl_vulkan_guest_active()) {
        int w = l->desc.texture_size.w, h = l->desc.texture_size.h;
        // The desc format is the guest's own request, echoed back to us through
        // ovrp_SetupLayer. Only the sRGB-ness matters for the allocation; the
        // channel order is fixed because that is what klovrp_gl_format's table
        // says this format is (measured here: fmt=0 -> R8G8B8A8_sRGB).
        const char *fname = NULL;
        uint32_t glfmt = klovrp_gl_format(l->desc.format, &fname);
        int srgb = (glfmt == 0x8C43);              // GL_SRGB8_ALPHA8
        // Array layout: ONE image for the stage with a layer per eye, and both
        // eyes get the same handle. Unity addresses the eye with
        // renderParams[].textureArraySlice, which is the provider's business
        // and not ours.
        int layers = (l->desc.layout == KLOVRP_LAYOUT_ARRAY) ? 2 : 1;
        // Keyed on the LAYER as well as the stage and eye. It was not, and every
        // layer the guest set up was handed the eye layer's images — invisible
        // for Unity, whose only other layer is a 1x1 dummy nothing renders into,
        // and the whole of RE4's broken intro logos, which are OculusHMD FSplash
        // quads that were being drawn into the corner of the eye texture.
        unsigned long long img =
            kl_vulkan_layer_image(l->is_eye ? KLVK_EYE_LAYER : layer_id,
                                  stage, eye, (unsigned)w, (unsigned)h, srgb, layers);
        if (!img) {
            fprintf(stderr, "  [ovrp] GetLayerTexture2(layer %d stage %d eye %d): "
                            "no VkImage — the guest is on Vulkan and the layer "
                            "storage could not be allocated\n",
                    layer_id, stage, eye);
            return OVRP_FAIL_NOT_INITIALIZED;
        }
        *color = img;
        if (l->is_eye) { g_eye_storage_w = w; g_eye_storage_h = h; }
        fprintf(stderr, "  [ovrp] GetLayerTexture2: %s%d eye %d stage %d = VkImage %#llx "
                        "(%dx%d %s%s)\n",
                l->is_eye ? "eye layer " : "layer ", layer_id, eye, stage, img, w, h,
                fname ? fname : "?",
                layers > 1 ? ", array slice — both eyes share this image" : "");
        return OVRP_SUCCESS;
    }

    uint32_t *slot = &l->tex[stage][eye];
    if (!*slot) {
        static void (*gl_GenTextures)(int32_t, uint32_t *);
        static void (*gl_BindTexture)(uint32_t, uint32_t);
        static void (*gl_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
        if (!gl_GenTextures) {
            gl_GenTextures  = kl_egl_sym("glGenTextures");
            gl_BindTexture  = kl_egl_sym("glBindTexture");
            gl_TexStorage2D = kl_egl_sym("glTexStorage2D");
        }
        uint32_t name = 0;
        if (gl_GenTextures) gl_GenTextures(1, &name);
        if (!name) {
            fprintf(stderr, "  [ovrp] GetLayerTexture2(layer %d stage %d eye %d): "
                            "glGenTextures produced no name — no GL context here\n",
                    layer_id, stage, eye);
            return OVRP_FAIL_NOT_INITIALIZED;
        }
        *slot = name;
        // The size is the one WE put in the desc and the provider passed back
        // through ovrp_SetupLayer, so it is used as declared — w wide, h tall.
        // klovrp_SetupEyeTexture2 allocates its 1.28 textures TRANSPOSED, and
        // that is not an inconsistency to fix here: there the size arrives from
        // libunity's legacy VRDevice and the guest's own resolve blit was
        // measured writing an (h, w) region, so the blit rect is ground truth
        // and overrides the arguments. Here nothing else has an opinion — the
        // provider hands this texture to Unity as desc.TextureSize — so
        // transposing would be inventing a disagreement.
        int w = l->desc.texture_size.w, h = l->desc.texture_size.h;
        // The format the guest asked for in the desc, honoured. Allocating
        // anything else is a resolve blit the driver refuses and a black eye
        // texture with the scene fully drawn behind it (klovrp_gl_format).
        const char *fname = NULL;
        uint32_t glfmt = klovrp_gl_format(l->desc.format, &fname);
        if (!glfmt) {
            glfmt = KL_OVRP_TEXFMT_EYE;
            fprintf(stderr, "  [ovrp] GetLayerTexture2: ovrpTextureFormat %d is not "
                            "mapped — allocating GL %#x and saying so, because a "
                            "format mismatch here is a silent black frame\n",
                    l->desc.format, glfmt);
        }
        // P5: the storage IS a compositor MTLTexture when one is on offer,
        // exactly as in klovrp_SetupEyeTexture2. Which of the two storages the
        // name got is a branch INSIDE the creation, not a second way out of
        // this function: the write to *color below is the whole point of the
        // call, and an early return that skipped it handed the guest its own
        // uninitialised stack slot as a texture name — a garbage attachment,
        // GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, and an eye texture nothing ever
        // rendered into, on the P5 path only — so every host run without a
        // provider was correct and it read as a compositor bug.
        int mtl_backed = 0;
        if (l->is_eye) {
            kl_glfb_note_eye_texture(eye, stage, name);
            mtl_backed = kl_glfb_has_mtl_provider() &&
                         kl_glfb_bind_eye_mtl_texture(eye, stage, name, w, h, glfmt);
        }
        if (!mtl_backed && gl_BindTexture && gl_TexStorage2D) {
            gl_BindTexture(0x0DE1 /* GL_TEXTURE_2D */, name);
            gl_TexStorage2D(0x0DE1, 1, glfmt, w, h);
        }
        // What the eye storage now IS, which is what makes the reuse in
        // klovrp_SetupLayer honest — see the comment there. kl_glfb keys the eye
        // textures by (eye, stage) and nothing else, so this call has just
        // OVERWRITTEN whatever the previous eye layer put in those slots.
        if (l->is_eye) { g_eye_storage_w = w; g_eye_storage_h = h; }
        fprintf(stderr, "  [ovrp] GetLayerTexture2: %s %d stage %d = GL %u "
                        "(%dx%d %s / GL %#x%s)\n",
                l->is_eye ? "eye" : "dummy layer", eye, stage, name, w, h,
                fname ? fname : "?", glfmt,
                mtl_backed ? ", MTLTexture-backed" : "");
    }
    *color = *slot;
    return OVRP_SUCCESS;
}

// ovrp_GetLayerTextureFoveation(layerId, stage, eye, uint64_t* tex,
//                               uint64_t* size) — real 0x16e240, which requires
// BOTH out pointers (two consecutive `cbz`es to -1001) and, like
// GetLayerTexture2 above, hands back 64-bit handles.
//
// Refused, and the refusal is the honest answer rather than a gap: this asks for
// a fixed-foveated-rendering DENSITY MAP TEXTURE, the Qualcomm
// QCOM_texture_foveated shape, which nothing on this host produces. Klepton
// does foveate — an MTLRasterizationRateMap attached inside ANGLE's Metal
// backend (KL_VRR, `make vrr`) — but that happens entirely below the guest and
// there is no texture in it to hand out. Saying so keeps one story: we already
// answer ovrp_GetTiledMultiResSupported false for the same reason.
//
// It is also a supported outcome for the caller, which is why this is a refusal
// rather than an abort-by-name: OculusDisplayProvider::CreateTexture tests the
// result (`cbnz w0`) and, on failure, simply does not attach a foveation texture
// to the Unity render-texture descriptor and carries on to create it. Answering
// SUCCESS with a zero handle would be the damaging answer — it sets the
// descriptor's "has foveation" flag against texture id 0.
static uint64_t klovrp_GetLayerTextureFoveation(int layer_id, int stage, int eye,
                                                uint64_t *tex, uint64_t *size) {
    ovrp_hit("ovrp_GetLayerTextureFoveation");
    (void)layer_id; (void)stage; (void)eye;
    if (!tex || !size) return OVRP_FAIL_INVALID_PARAM;
    *tex = 0;
    *size = 0;
    return OVRP_FAIL_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// The CPU/GPU performance levels — recorded, not applied
// ---------------------------------------------------------------------------
// OVRManager pushes a CPU and a GPU level (Quest's 0..3 clock hints) and 1.40's
// plugin reads them back. They are RECORDED here and nothing acts on them, for
// the same reason Process.setThreadPriority is recorded in kl_jni: Darwin sets
// scheduling through pthread QoS on the thread itself, and there is no
// per-app GPU clock hint at all.
//
// The read-back answers what the setter stored rather than a constant, because a
// getter that disagrees with the setter is what trap 10's neighbours are made of
// — the guest sets a level, reads a different one back, and concludes the
// request was rejected. The un-suffixed setters go through the same store, so
// there is one value and not two.
//
// The initial value is the mid level, 2, which is the Quest 2 default and
// therefore agrees with the device we describe everywhere else. It is a
// stand-in, and it is only ever visible in the window before the guest sets one.
static int g_cpu_level = 2;
static int g_gpu_level = 2;

static uint64_t klovrp_SetSystemCpuLevel(int level) {
    ovrp_hit("ovrp_SetSystemCpuLevel");
    g_cpu_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetSystemCpuLevel2(int level) {
    ovrp_hit("ovrp_SetSystemCpuLevel2");
    g_cpu_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSystemCpuLevel2(int *out) {
    ovrp_hit("ovrp_GetSystemCpuLevel2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_cpu_level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetSystemGpuLevel(int level) {
    ovrp_hit("ovrp_SetSystemGpuLevel");
    g_gpu_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetSystemGpuLevel2(int level) {
    ovrp_hit("ovrp_SetSystemGpuLevel2");
    g_gpu_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSystemGpuLevel2(int *out) {
    ovrp_hit("ovrp_GetSystemGpuLevel2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_gpu_level;
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// The compositor-telemetry group
// ---------------------------------------------------------------------------
// 1.40's plugin publishes Unity's XR stats every frame, which means asking
// OVRPlugin how long the compositor took. There is no compositor here in the
// sense these questions mean — no VrApi frame submission with a measured GPU
// end and a vsync to be early or late for — so every one of them is refused
// rather than answered with a plausible number. This is a GROUP answer on
// purpose (CLAUDE.md's rule): a per-call mix of invented milliseconds would let
// the guest derive a frame budget from figures that do not describe anything,
// and the numbers would silently disagree with each other.
//
// The guest is built for the refusal: OculusSystem's own wrappers
// (GetCompositorCPUTime, GetCompositorCPUStartToGPUEndTime,
// GetGPUEndToVsyncElapsedTime) test the sign of the result and answer 0.0f when
// it is negative, so a refusal is a path it already takes on hardware whose
// runtime does not report these.
//
// Every out-param is still written before returning: a caller that ignores the
// result and reads the slot gets a NEUTRAL value rather than whatever was on its
// stack — 0 for a duration, and 1.0 for a SCALE, where 0 would read as "shrink
// the render target to nothing".
static uint64_t klovrp_GetAppCpuStartToGpuEndTime2(float *out) {
    ovrp_hit("ovrp_GetAppCpuStartToGpuEndTime2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0.0f;
    return OVRP_FAIL_UNSUPPORTED;
}

static uint64_t klovrp_GetAdaptiveGpuPerformanceScale2(float *out) {
    ovrp_hit("ovrp_GetAdaptiveGpuPerformanceScale2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1.0f;                      // neutral scale — see the note above
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_IsPerfMetricsSupported(metric, ovrpBool* out) — real 0x1715d0, -1001 on a
// NULL x1. Unlike the getters this one IS answerable, and the answer is false:
// we publish no performance metrics at all, so the guest never asks for one.
static uint64_t klovrp_IsPerfMetricsSupported(int metric, int *out) {
    ovrp_hit("ovrp_IsPerfMetricsSupported");
    (void)metric;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetPerfMetricsFloat(int metric, float *out) {
    ovrp_hit("ovrp_GetPerfMetricsFloat");
    (void)metric;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0.0f;
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_GetAppPerfStats2(ovrpAppPerfStats* out) — real 0x171480: -1001 on NULL,
// otherwise it memcpy's **292 bytes (0x124)** into the caller's buffer and
// returns 0. That length is the one thing here worth taking from the
// disassembly rather than from a struct definition, because it is what makes
// zeroing the buffer safe: the caller memsets the same 292 bytes before the
// call, then reads an int at +0x120 and uses it to index 0x38-byte entries from
// the base, so an all-zero block is a coherent "no frame statistics", not a
// half-written one.
//
// Refused for the same reason as the rest of the telemetry group, and the
// caller's own wrapper answers 0.0f on a negative result. Distinct from
// ovrp_GetAppPerfStats (the un-numbered 1.28 form), which is not an out-param
// call at all — libunity reads the RETURNED pointer there.
#define KLOVRP_APP_PERF_STATS_BYTES 292
static uint64_t klovrp_GetAppPerfStats2(void *out) {
    ovrp_hit("ovrp_GetAppPerfStats2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    memset(out, 0, KLOVRP_APP_PERF_STATS_BYTES);
    return OVRP_FAIL_UNSUPPORTED;
}

static uint64_t klovrp_GetPerfMetricsInt(int metric, int *out) {
    ovrp_hit("ovrp_GetPerfMetricsInt");
    (void)metric;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_GetViewportStencil(eye, type, ovrpVector2f* verts, int* vertexCount,
//                         uint16_t* indices, int* indexCount) — real 0x171bf0,
// whose register shuffle (x5->x7, x4->x6, x3->x5, x2->x4, w1->w2, w0->w1) is
// what fixes this order. The provider calls it twice per eye from
// SetupOcclusionMesh: once with NULL buffers to size them, then with buffers.
//
// Refused, for the same reason ovrp_GetEyeOcclusionMesh already answers false:
// the viewport stencil is the headset's own hidden-area mesh, a property of a
// physical lens assembly that is not here. There is nothing to hand over and
// nothing to derive it from, and SetupOcclusionMesh tests the sign of the result
// (`tbnz w0, #0x1f`) before touching the counts, so a refusal is a path the
// guest already has.
static uint64_t klovrp_GetViewportStencil(int eye, int type, void *verts,
                                          int *vertex_count, void *indices,
                                          int *index_count) {
    ovrp_hit("ovrp_GetViewportStencil");
    (void)eye; (void)type; (void)verts; (void)indices;
    // Zeroed as well as refused: the guest allocates from these counts on the
    // success path, and a stale count next to a failure is the shape that turns
    // one wrong branch into an allocation.
    if (vertex_count) *vertex_count = 0;
    if (index_count)  *index_count  = 0;
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_CalculateEyeViewportRect(const ovrpLayerDesc*, ovrpEye, ovrpRecti* out,
//                               float scale) — real 0x16e730, -1001 on a NULL
// x2, scale in s0. The viewport an eye occupies inside the layer's texture.
//
// One eye per texture in every layout this guest asks for (Stereo gives two
// separate textures, Array two slices), so the eye's viewport is the whole
// thing — scaled by the render-viewport scale the caller passes, which is how
// Unity's dynamic resolution reaches the layer.
//
// **KL_OVRP_VIEWPORT_SCALE forces that scale, and it is the A/B this correction
// had no way to run.** The whole render-viewport path is the guest's own
// decision — Beat Saber shrinks it on entering a map — so on the host it is
// reached only by playing to that point, and measured host runs never reach it
// at all: every `[ovrp] render viewport` line in a viewer log so far reads
// 100.0%. That left the compositors' crop untested by anything but the CPU
// arithmetic in `make reproject`, and a device-only symptom with no host repro.
//
// It is forced HERE, and not by writing a rect into the frame record, on
// purpose: this entry point is where the guest ASKS where to render. Answering
// smaller makes it really set that GL viewport, really render into that
// sub-rect, and really submit it at ovrp_EndFrame4 — so the knob exercises the
// whole chain rather than the last link of it. A correct composite shows the
// SAME picture at any scale, merely softer; a wrong one puts it in a corner or
// stretches it, which is the failure being hunted.
static float klovrp_viewport_scale(void) {
    static float s = -1.0f;
    if (s < 0.0f) {
        s = kl_env_float("KL_OVRP_VIEWPORT_SCALE", 1.0f);
        if (!(s > 0.0f) || s > 1.0f) s = 1.0f;
        if (s != 1.0f)
            fprintf(stderr, "  [ovrp] KL_OVRP_VIEWPORT_SCALE=%.3f — the guest is "
                            "told to render into that fraction of its eye "
                            "texture, whatever it asked for\n", (double)s);
    }
    return s;
}

static uint64_t klovrp_CalculateEyeViewportRect(const ovrp_layer_desc_eyefov *desc,
                                                int eye, ovrp_recti *out, float scale) {
    ovrp_hit("ovrp_CalculateEyeViewportRect");
    (void)eye;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    int w = desc ? desc->texture_size.w : 0, h = desc ? desc->texture_size.h : 0;
    if (!(scale > 0.0f) || scale > 1.0f) scale = 1.0f;
    // Multiplied, not replaced: a guest already scaling to 0.8 and a knob of
    // 0.5 means half of what it chose, so the knob cannot accidentally make the
    // viewport LARGER than the guest's own decision — which would be a rect the
    // guest never rendered into and a black border rather than a crop bug.
    scale *= klovrp_viewport_scale();
    w = (int)((float)w * scale);
    h = (int)((float)h * scale);
    *out = (ovrp_recti){ 0, 0, w > 0 ? w : 1, h > 0 ? h : 1 };
    return OVRP_SUCCESS;
}

// ovrp_CalculateEyePreviewRect(const ovrpLayerDesc*, ovrpEye, const ovrpRecti*
//                              viewport, ovrpRectf* out) — real 0x16e810,
// -1001 on a NULL x3, and it writes 16 bytes (`stp x8, x1, [x19]`), i.e. four
// floats. The viewport expressed in the texture's normalised space, which for a
// viewport that IS the texture is the unit rect.
static uint64_t klovrp_CalculateEyePreviewRect(const ovrp_layer_desc_eyefov *desc,
                                               int eye, const ovrp_recti *viewport,
                                               ovrp_rectf *out) {
    ovrp_hit("ovrp_CalculateEyePreviewRect");
    (void)eye;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
    if (desc && viewport && desc->texture_size.w > 0 && desc->texture_size.h > 0) {
        float tw = (float)desc->texture_size.w, th = (float)desc->texture_size.h;
        x = (float)viewport->x / tw;
        y = (float)viewport->y / th;
        w = (float)viewport->w / tw;
        h = (float)viewport->h / th;
    }
    *out = (ovrp_rectf){ x, y, w, h };
    return OVRP_SUCCESS;
}

// The other half of SetupEyeTexture2, and for a long time a no-op — which is
// what made the eye swapchain a per-loading-transition leak. Unity re-creates
// the swapchain whenever the eye size changes (measured: three times before the
// menu is even up), and each generation is three stages of a two-slice RGBA16F
// array — 250-380 MiB. Nothing else releases it: the guest never calls
// glDeleteTextures on the names it hands down here, because on a real Quest the
// storage belongs to VrApi and *this call* is where it dies. So it has to die
// here too, or every transition leaks a whole swapchain (§12.21).
//
// Two ints, x0 and x1, passed straight through from libunity's wrapper at
// 0x9bbbdc; the return is ignored (it answers 1 unconditionally either way).
// Which of the two is the eye and which the stage is measured, not assumed —
// kl_glfb_release_eye_texture takes them in the order this call site uses and
// refuses anything out of range rather than releasing the wrong slot.
static uint64_t klovrp_DestroyEyeTexture(int eye, int stage) {
    ovrp_hit("ovrp_DestroyEyeTexture");
    fprintf(stderr, "  [ovrp] DestroyEyeTexture(eye=%d stage=%d)\n", eye, stage);
    kl_glfb_release_eye_texture(eye, stage);
    return OVRP_SUCCESS;
}

// libunity reads the RETURNED pointer's +0x118/+0x11c first and early-outs
// when they are zero (0x9bcc58) — an out-param it is not: returning 0 here
// segfaulted at [x0+0xcc]. A zeroed static struct is the complete honest
// answer: no perf stats, because there is no compositor producing them.
static uint64_t klovrp_GetAppPerfStats(void) {
    static char stats[0x120];
    ovrp_hit("ovrp_GetAppPerfStats");
    return (uint64_t)(uintptr_t)stats;
}

// Optional depth-compositing probe (slot null-checked, 0x9bced4). We do not
// composite depth, so both the return and the out-int say so.
static uint64_t klovrp_GetDepthCompositingSupported(int *out) {
    ovrp_hit("ovrp_GetDepthCompositingSupported");
    *out = 0;
    return 0;
}

// Mixed-reality capture — the camera composite an Oculus device does for
// spectators. It takes no arguments and returns `ovrpBool`, not `ovrpResult`, so
// 0 is FALSE and is the answer we want (trap 10 is the reason to say that out
// loud: the two types disagree about which value means yes, and 0 looks like the
// wrong one here). There is no capture camera, `ovrp_InitializeMixedReality` is
// never answered, and the guest polls this before deciding whether to build the
// composite path at all.
static uint64_t klovrp_GetMixedRealityInitialized(void) {
    ovrp_hit("ovrp_GetMixedRealityInitialized");
    return 0;
}

// Unity calls this on every native plugin it loads, handing over its
// IUnityInterfaces registry. The real OVRPlugin uses it to grab
// IUnityGraphicsVulkan/GLES; we record it and do nothing, which is correct until
// there is a renderer to bind to (PLANNING M5/M6).
static void klovrp_UnityPluginLoad(void *unity_interfaces) {
    fprintf(stderr, "  [ovrp] UnityPluginLoad(%p) — recorded; no graphics device "
                    "bound yet\n", unity_interfaces);
}
static void klovrp_UnityPluginUnload(void) {}

// The real OVRPlugin exports JNI_OnLoad and caches the JavaVM out of it. Ours
// has no JNI surface to set up — every entry point here is answered from
// kl_ovrp's own state, and the VM is reachable through kl_jni_vm() anywhere it
// were ever needed — so the whole body is the version number Android checks.
//
// It is reached at all only because System.load() now honours the Android
// contract and calls JNI_OnLoad on what it loaded (kl_jni.c). Answering the
// version is not a stub: returning nothing, or a version Android does not
// recognise, is how a real library reports that it refused to initialize.
static int klovrp_JNI_OnLoad(void *vm, void *reserved) {
    (void)vm; (void)reserved;
    return 0x00010006;                 // JNI_VERSION_1_6, as the real one returns
}

// BONELAB / the Vulkan path. libOculusXRPlugin asks OVRPlugin which Vulkan
// extensions the *runtime* needs before it creates the instance and the device,
// and hands the answer straight to vkCreateInstance / vkCreateDevice.
//
// **It is an array of STRING POINTERS, not a character buffer**, and getting
// that wrong is trap 10b's family in a new API — a `strlen` of whatever the
// caller's uninitialised slot happened to hold, on a thread whose crash report
// names nothing. It cost one run to find and the evidence is worth keeping,
// because the two readings are indistinguishable from the real plugin alone:
//
//   0x126f50 <ovrp_GetInstanceExtensionsVk>:
//     cbz  x1, +0x28        -> mov w0, #-1001   (invalid parameter)
//     ...initialized?       -> mov w0, #-1003
//     and  w0, w0, w0, asr #31                  (plain ovrpResult)
//
// That says x1 is required and x0 is not checked — consistent with BOTH
// `(char *buf, uint32_t *cap)` and the truth. What settles it is the CALLER,
// whose C++ name survived in libOculusXRPlugin.so:
//
//   OculusSystem::GetVulkanExtensions(void*, unsigned, unsigned*, char*,
//                                     ovrpResult (*)(char const**, int*))
//
// `PPKc` is `const char **` and `Pi` is `int *`. Its loop reads the argument
// back as `ldr x0, [x8, x23, lsl #3]` — an 8-byte stride — and hands each
// element to `strlen`. So x0 is an array of pointers the caller owns, and
// writing a NUL "string" into it corrupts a pointer rather than emptying a
// buffer.
//
// The answer is NO extensions, and that is a measurement rather than a
// convenience. On a Quest this list is how the Oculus runtime says which
// external-memory extensions it needs in order to share eye textures with its
// compositor. Here the "runtime" is this file plus MoltenVK, and it shares
// nothing through Vulkan — so requiring anything would be inventing a
// constraint, and inventing one MoltenVK does not have would fail the guest's
// own vkCreateInstance. A count of 0 makes the caller's `cmp w8, #1 / b.lt`
// skip the whole loop, so no element is ever read.
static int32_t klovrp_GetInstanceExtensionsVk(const char **names, int *count) {
    (void)names;
    if (!count) return OVRP_FAIL_INVALID_PARAM;
    *count = 0;
    return OVRP_SUCCESS;
}

static int32_t klovrp_GetDeviceExtensionsVk(const char **names, int *count) {
    (void)names;
    if (!count) return OVRP_FAIL_INVALID_PARAM;
    *count = 0;
    return OVRP_SUCCESS;
}

// The hand-SKELETON family (BONELAB). Distinct from the node poses this file
// already answers: this is the 64-bone finger rig, asked for once at startup.
//
//   0x12b3c0 <ovrp_GetSkeleton2>:
//     mov w1, w0 / cmp w1, #3 / b.hi -> -1001     (skeletonType, 0..3)
//     cbz x2 -> -1001                             (the out struct is required)
//     ...no singleton -> -1002
//
// so it is `ovrpResult f(ovrpSkeletonType, ovrpSkeleton2 *)`.
//
// **Refused, and deliberately without writing the out struct.** Every other
// out-parameter in this file is filled — trap 10b is precisely about the cost of
// not doing so — but that rule cannot be followed here honestly: `ovrpSkeleton2`
// is a large struct whose layout is NOT available anywhere in this APK (no DWARF
// for it, and the C# side is IL2CPP'd), so zeroing it would mean guessing a size
// and memset-ing that many bytes of the caller's stack. A guessed size is a
// stack smash; a refusal is a code path the caller already has.
//
// It is safe here because the caller CHECKS: OVRPlugin's C# wrapper is
// `return ovrp_GetSkeleton2(t, out s) == Result.Success`, so a negative result
// is a `false` return and the guest skips its skeleton setup. If a future guest
// ignores the result, the symptom will be garbage bones rather than a crash, and
// the fix is to find the layout — not to invent one.
// The whole family answers the same way, so it is one function rather than one
// per entry point. That is normally exactly what trap 10 warns against — a
// numbered suffix marks an ABI change, so `ovrp_Foo` and `ovrp_Foo2` must not be
// assumed to share a shape. It is safe HERE and only here because this handler
// reads no argument and writes no out-parameter: it returns a constant negative
// whatever the calling convention is, so there is no shape to get wrong.
//
// Each name is still listed explicitly rather than matched by prefix, so a new
// `ovrp_Hand*` in a future SDK lands on the fail-closed abort-by-name and gets
// classified deliberately.
static int32_t klovrp_hand_unsupported(void) { return OVRP_FAIL_UNSUPPORTED; }

static int32_t klovrp_GetSkeleton2(int skeleton_type, void *out) {
    if (skeleton_type < 0 || skeleton_type > 3 || !out) return OVRP_FAIL_INVALID_PARAM;
    return OVRP_FAIL_UNSUPPORTED;
}

static const char g_ovrp_handle[] = "klepton-ovrplugin";

// Assembly entry thunks that capture the x8 sret pointer before any call can
// clobber it (kl_ovrp_sret.S); the _impl bodies are above.
void klovrp_GetNodePoseState_entry(void);
void klovrp_GetControllerState2_entry(void);
void klovrp_GetControllerHapticsDesc_entry(void);
void klovrp_GetControllerState_entry(void);

int kl_ovrp_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    // Both spellings occur: Unity asks the ClassLoader for "OVRPlugin" and then
    // dlopens whatever path came back.
    return strcmp(b, "libOVRPlugin.so") == 0 || strcmp(b, "OVRPlugin") == 0;
}

void *kl_ovrp_dlopen(const char *soname) {
    if (!kl_ovrp_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [ovrp] guest dlopen(\"%s\") -> synthetic OVRPlugin "
                    "(the real one NEEDs libvrapi.so; see PLANNING 3.1)\n", b);
    return (void *)g_ovrp_handle;
}

int kl_ovrp_is_handle(const void *h) { return h == (const void *)g_ovrp_handle; }

static const struct { const char *name; void *fn; } g_ovrp_impl[] = {
    {"UnityPluginLoad",   (void *)klovrp_UnityPluginLoad},
    {"UnityPluginUnload", (void *)klovrp_UnityPluginUnload},
    {"JNI_OnLoad",        (void *)klovrp_JNI_OnLoad},
    {"ovrp_GetSkeleton2", (void *)klovrp_GetSkeleton2},
    // ...and the rest of the hand-tracking surface, refused as a group. See
    // klovrp_hand_unsupported for why one handler may serve numbered siblings
    // here when it may not anywhere else in this file.
    {"ovrp_GetSkeleton",  (void *)klovrp_hand_unsupported},
    {"ovrp_GetSkeleton3", (void *)klovrp_hand_unsupported},
    {"ovrp_GetHandState",  (void *)klovrp_hand_unsupported},
    {"ovrp_GetHandState2", (void *)klovrp_hand_unsupported},
    {"ovrp_GetMesh",  (void *)klovrp_hand_unsupported},
    {"ovrp_GetInstanceExtensionsVk", (void *)klovrp_GetInstanceExtensionsVk},
    {"ovrp_GetDeviceExtensionsVk",   (void *)klovrp_GetDeviceExtensionsVk},
    // The init state and everything that reads it. See klovrp_GetInitialized.
    {"ovrp_Initialize5",   (void *)klovrp_Initialize5},
    {"ovrp_Initialize7",   (void *)klovrp_Initialize7},
    {"ovrp_GetInitialized", (void *)klovrp_GetInitialized},
    {"ovrp_Shutdown",      (void *)klovrp_Shutdown},
    {"ovrp_GetAppChromaticCorrection", (void *)klovrp_GetAppChromaticCorrection},
    {"ovrp_SetAppChromaticCorrection", (void *)klovrp_SetAppChromaticCorrection},
    {"ovrp_SetAppEngineInfo",  (void *)klovrp_SetAppEngineInfo},
    {"ovrp_SetAppEngineInfo2", (void *)klovrp_SetAppEngineInfo2},
    {"ovrp_GetAppShouldQuit2", (void *)klovrp_GetAppShouldQuit2},
    {"ovrp_GetAppShouldRecreateDistortionWindow2",
     (void *)klovrp_GetAppShouldRecreateDistortionWindow2},
    {"ovrp_GetSystemRecommendedMSAALevel2", (void *)klovrp_GetSystemRecommendedMSAALevel2},
    {"ovrp_GetGPUFrameTime",   (void *)klovrp_GetGPUFrameTime},
    {"ovrp_InitializeMixedReality", (void *)klovrp_InitializeMixedReality},
    {"ovrp_SetControllerVibration2", (void *)klovrp_SetControllerVibration2},
    {"ovrp_GetVersion",   (void *)klovrp_GetVersion},
    {"ovrp_GetVersion2",   (void *)klovrp_GetVersion2},
    {"ovrp_GetNativeSDKVersion",  (void *)klovrp_GetNativeSDKVersion},
    {"ovrp_GetNativeSDKVersion2", (void *)klovrp_GetNativeSDKVersion2},
    {"ovrp_GetSystemHeadsetType", (void *)klovrp_GetSystemHeadsetType},
    {"ovrp_GetSystemHeadsetType2", (void *)klovrp_GetSystemHeadsetType2},
    {"ovrp_GetLocalTrackingSpaceRecenterCount",
     (void *)klovrp_GetLocalTrackingSpaceRecenterCount},
    {"ovrp_GetFoveationEyeTrackedSupported",
     (void *)klovrp_GetFoveationEyeTrackedSupported},
    {"ovrp_GetSystemMultiViewSupported2", (void *)klovrp_GetSystemMultiViewSupported2},
    {"ovrp_GetSystemMultiViewSupported",  (void *)klovrp_GetSystemMultiViewSupported},
    {"ovrp_GetEyeTextureArraySupported2", (void *)klovrp_GetEyeTextureArraySupported2},
    {"ovrp_GetEyeTextureArraySupported",  (void *)klovrp_GetEyeTextureArraySupported},
    {"ovrp_GetBoundaryConfigured2", (void *)klovrp_GetBoundaryConfigured2},
    {"ovrp_GetAppHasVrFocus2", (void *)klovrp_GetAppHasVrFocus2},
    {"ovrp_GetUserPresent2", (void *)klovrp_GetUserPresent2},
    {"ovrp_GetNodePresent2", (void *)klovrp_GetNodePresent2},
    {"ovrp_GetNodeOrientationTracked2", (void *)klovrp_GetNodeOrientationTracked2},
    {"ovrp_GetSystemBatteryTemperature2", (void *)klovrp_GetSystemBatteryTemperature2},
    {"ovrp_GetSystemBatteryLevel2", (void *)klovrp_GetSystemBatteryLevel2},
    {"ovrp_GetSystemPowerSavingMode2", (void *)klovrp_GetSystemPowerSavingMode2},
    {"ovrp_GetAppShouldRecenter2", (void *)klovrp_GetAppShouldRecenter2},
    {"ovrp_GetPredictedDisplayTime", (void *)klovrp_GetPredictedDisplayTime},
    {"ovrp_GetSystemProductName", (void *)klovrp_GetSystemProductName},
    {"ovrp_GetSystemProductName2", (void *)klovrp_GetSystemProductName2},
    {"ovrp_GetSystemDisplayFrequency", (void *)klovrp_GetSystemDisplayFrequency},
    {"ovrp_GetSystemDisplayFrequency2", (void *)klovrp_GetSystemDisplayFrequency2},
    {"ovrp_SetSystemDisplayFrequency", (void *)klovrp_SetSystemDisplayFrequency},
    {"ovrp_GetEyeTextureSize", (void *)klovrp_GetEyeTextureSize},
    {"ovrp_GetEyeTextureStageCount", (void *)klovrp_GetEyeTextureStageCount},
    {"ovrp_GetDesiredEyeTextureFormat", (void *)klovrp_GetDesiredEyeTextureFormat},
    {"ovrp_GetTiledMultiResLevel", (void *)klovrp_GetTiledMultiResLevel},
    {"ovrp_SetTrackingOriginType", (void *)klovrp_SetTrackingOriginType},
    {"ovrp_SetTrackingOriginType2", (void *)klovrp_SetTrackingOriginType2},
    {"ovrp_GetTrackingOriginType", (void *)klovrp_GetTrackingOriginType},
    {"ovrp_GetTrackingOriginType2", (void *)klovrp_GetTrackingOriginType2},
    {"ovrp_PollEvent",  (void *)klovrp_PollEvent},
    {"ovrp_PollEvent2", (void *)klovrp_PollEvent2},
    {"ovrp_GetNodeFrustum2", (void *)klovrp_GetNodeFrustum2},
    // Real implementations only so that the frame boundary can be *observed* —
    // both still answer ovrpSuccess and neither has an out-param. This is the
    // timewarp bookkeeping (kl_ovrp.h): the pose the guest is about to render
    // with, latched against the stage that frame draws into.
    {"ovrp_BeginFrame", (void *)klovrp_BeginFrame},
    {"ovrp_BeginFrame4", (void *)klovrp_BeginFrame4},
    {"ovrp_EndFrame", (void *)klovrp_EndFrame},
    {"ovrp_GetNodePositionTracked2", (void *)klovrp_GetNodePositionTracked2},
    {"ovrp_GetNodePositionValid", (void *)klovrp_GetNodePositionValid},
    {"ovrp_GetNodeOrientationValid", (void *)klovrp_GetNodeOrientationValid},
    {"ovrp_GetNodePoseState", (void *)klovrp_GetNodePoseState_entry},
    {"ovrp_GetNodePoseState3", (void *)klovrp_GetNodePoseState3},
    {"ovrp_WaitToBeginFrame", (void *)klovrp_WaitToBeginFrame},
    // The 1.40 layer family — see the block comment above klovrp_fill_eye_desc.
    {"ovrp_CalculateEyeLayerDesc2", (void *)klovrp_CalculateEyeLayerDesc2},
    {"ovrp_CalculateEyeLayerDesc3", (void *)klovrp_CalculateEyeLayerDesc3},
    {"ovrp_CalculateLayerDesc", (void *)klovrp_CalculateLayerDesc},
    {"ovrp_SetupLayer", (void *)klovrp_SetupLayer},
    {"ovrp_DestroyLayer", (void *)klovrp_DestroyLayer},
    {"ovrp_GetLayerTextureStageCount", (void *)klovrp_GetLayerTextureStageCount},
    {"ovrp_GetLayerTexture2", (void *)klovrp_GetLayerTexture2},
    {"ovrp_GetLayerTextureFoveation", (void *)klovrp_GetLayerTextureFoveation},
    {"ovrp_GetViewportStencil", (void *)klovrp_GetViewportStencil},
    {"ovrp_EndFrame4", (void *)klovrp_EndFrame4},
    {"ovrp_Update3", (void *)klovrp_Update3},
    {"ovrp_GetUserIPD2", (void *)klovrp_GetUserIPD2},
    {"ovrp_GetUserIPD", (void *)klovrp_GetUserIPD},
    {"ovrp_GetAppCpuStartToGpuEndTime2", (void *)klovrp_GetAppCpuStartToGpuEndTime2},
    {"ovrp_GetAdaptiveGpuPerformanceScale2", (void *)klovrp_GetAdaptiveGpuPerformanceScale2},
    {"ovrp_IsPerfMetricsSupported", (void *)klovrp_IsPerfMetricsSupported},
    {"ovrp_GetPerfMetricsFloat", (void *)klovrp_GetPerfMetricsFloat},
    {"ovrp_GetPerfMetricsInt", (void *)klovrp_GetPerfMetricsInt},
    {"ovrp_GetAppPerfStats2", (void *)klovrp_GetAppPerfStats2},
    {"ovrp_SetSystemCpuLevel", (void *)klovrp_SetSystemCpuLevel},
    {"ovrp_SetSystemCpuLevel2", (void *)klovrp_SetSystemCpuLevel2},
    {"ovrp_GetSystemCpuLevel2", (void *)klovrp_GetSystemCpuLevel2},
    {"ovrp_SetSystemGpuLevel", (void *)klovrp_SetSystemGpuLevel},
    {"ovrp_SetSystemGpuLevel2", (void *)klovrp_SetSystemGpuLevel2},
    {"ovrp_GetSystemGpuLevel2", (void *)klovrp_GetSystemGpuLevel2},
    {"ovrp_CalculateEyeViewportRect", (void *)klovrp_CalculateEyeViewportRect},
    {"ovrp_CalculateEyePreviewRect", (void *)klovrp_CalculateEyePreviewRect},
    {"ovrp_GetAppPerfStats", (void *)klovrp_GetAppPerfStats},
    {"ovrp_GetBoundaryDimensions", (void *)klovrp_GetBoundaryDimensions},
    {"ovrp_GetControllerState2", (void *)klovrp_GetControllerState2_entry},
    {"ovrp_GetControllerState", (void *)klovrp_GetControllerState_entry},
    {"ovrp_GetControllerState4", (void *)klovrp_GetControllerState4},
    {"ovrp_GetAppAsymmetricFov", (void *)klovrp_GetAppAsymmetricFov},
    {"ovrp_GetAppHasInputFocus", (void *)klovrp_GetAppHasInputFocus},
    {"ovrp_GetNativeXrApiType", (void *)klovrp_GetNativeXrApiType},
    {"ovrp_GetSystemDisplayAvailableFrequencies", (void *)klovrp_GetSystemDisplayAvailableFrequencies},
    {"ovrp_SetupEyeTexture2", (void *)klovrp_SetupEyeTexture2},
    {"ovrp_DestroyEyeTexture", (void *)klovrp_DestroyEyeTexture},
    {"ovrp_Update2", (void *)klovrp_Update2},
    {"ovrp_GetControllerHapticsDesc", (void *)klovrp_GetControllerHapticsDesc_entry},
    // M8 — haptics out. All three must be real together: the descriptor sizes
    // the guest's buffer, the state paces it, and only then does it ever call
    // the setter. See the block comment above klovrp_SetControllerHaptics.
    {"ovrp_GetControllerHapticsState", (void *)klovrp_GetControllerHapticsState},
    {"ovrp_GetControllerHapticsDesc2", (void *)klovrp_GetControllerHapticsDesc2},
    {"ovrp_GetControllerHapticsState2", (void *)klovrp_GetControllerHapticsState2},
    {"ovrp_SetControllerHaptics", (void *)klovrp_SetControllerHaptics},
    {"ovrp_SetControllerVibration", (void *)klovrp_SetControllerVibration},
    // ...and 1.40's own pair, which libhaptics_sdk resolves directly. Both, or
    // neither: the SDK reads the rate before it will play, and refuses a rate
    // it cannot get ("the actuator sample rate is invalid").
    {"ovrp_GetControllerSampleRateHz", (void *)klovrp_GetControllerSampleRateHz},
    {"ovrp_SetControllerHapticsPcm", (void *)klovrp_SetControllerHapticsPcm},
    {"ovrp_GetDepthCompositingSupported", (void *)klovrp_GetDepthCompositingSupported},
    {"ovrp_GetMixedRealityInitialized", (void *)klovrp_GetMixedRealityInitialized},
};

// Entry points answered by one of the shared handlers above. Each is reached
// through the same per-name trampoline as the aborting handler, so x0 is the
// entry point's own name and kl_ovrp_report still counts them individually.
//
// Ignoring the arguments is ABI-safe whatever the real arity: under AAPCS64 the
// caller passes in registers and cleans up after itself, so a callee that reads
// none of them and returns a scalar cannot corrupt anything. That is what makes a
// shared handler viable without knowing all 466 signatures. It stops being true
// the moment an entry point has an *out-parameter* — those must know where the
// pointer is and what shape it points at, so they get real implementations.
static const char *const g_ovrp_result_ok[] = {
    // Unity's native plugin interface. All void.
    "UnitySetGraphicsDevice", "UnitySetEventQueue", "UnityShaderCompilerExtEvent",
    "UnityRenderingExtEvent",
    // ...and the audio-plugin enumeration, which libunity's AudioPluginManager
    // dlsyms speculatively on every native plugin handle it holds. It returns a
    // COUNT of effect definitions, so 0 is "this plugin publishes no audio
    // effects" — the same thing the real library says by not exporting it at
    // all, which on Android is a failed dlsym. Ours resolves everything by
    // design (a lookup is a measurement, kl_ovrp.h), so it has to answer.
    "UnityGetAudioEffectDefinitions",
    // Bring-up. This is the decision recorded in PLANNING M6: we answer success
    // and stand behind it, rather than reporting a failure Unity would be right
    // to believe.
    // ovrp_PreInitialize3 is the numbered sibling 1.40's libOculusXRPlugin
    // dlsyms (its own real implementation is a relay to ovrp_PreInitialize5 that
    // zeroes the three input args and returns ovrpResult 0 on first call, and on
    // THIS host this is always the first call). Numbered variants are listed
    // rather than matched by prefix — trap 10's whole shape is a numbered
    // variant that returns something DIFFERENT under a familiar name.
    // ovrp_Initialize5 and ovrp_Initialize7 answer the same success and have
    // moved to real implementations, because the answer has to be RECORDED —
    // see klovrp_GetInitialized.
    "ovrp_PreInitialize", "ovrp_PreInitialize3",
    // Configuration the guest sets and never reads back.
    "ovrp_SetAppAsymmetricFov",
    // Called with an out-pointer (void**) it may write; libunity pre-zeroes
    // the local and ignores the x0 return (0x9bb334-0x9bb414), and never
    // dereferences whatever lands in the slot — so leaving it untouched and
    // returning 0 stores NULL, which is the truthful "no native SDK here".
    "ovrp_GetNativeSDKPointer2",
    // The frame lifecycle (0x9bb808 dispatcher: BeginFrame, EndEye2 x2,
    // EndFrame) and the one-shot Update2 at the end of init (0x9bb52c) and
    // reconfigure (0x9bce3c). All have their return ignored by the guest and
    // take no out-params. BeginFrame and EndFrame have moved to real
    // implementations above — they answer the same ovrpSuccess, but they are
    // where the timewarp bookkeeping is latched.
    "ovrp_EndEye2",
    "ovrp_RecenterTrackingOrigin",
    // Thread-scheduling hints from PlayerSettings, set once at init; void
    // configuration like the setters above.
    "ovrp_AutoThreadScheduling", "ovrp_SetThreadPerformance",
    // ...and UE4's, which pushes one more of them (ovrp_SetAppCPUPriority2)
    // beside the CPU/GPU levels the Unity guests set. Same class: a hint, and
    // there is nothing on this host that acts on it.
    "ovrp_SetAppCPUPriority2",
    // Fixed-foveation level and its dynamic flag. We answer
    // ovrp_GetTiledMultiResSupported false and mean it — the foveation on this
    // host is the compositor's MTLRasterizationRateMap, which is not the thing
    // this switch controls — but a setter that REFUSES is a different statement
    // from an unsupported capability, and UE4 pushes both unconditionally
    // during InitializeSession. Recorded, like every other setter here. The
    // real bodies (+0x31918, +0x319a8) are the standard -1002-or-backend shape.
    "ovrp_SetTiledMultiResLevel", "ovrp_SetTiledMultiResDynamic",
    // Whether recentring the controller also recentres the head. Nothing here
    // recentres either, so this is recorded state with nothing behind it — and
    // its getter already answers through the same silence.
    "ovrp_SetReorientHMDOnControllerRecenter",
    // Called even with texture-array support answered 0 — recorded state,
    // like the other setters.
    "ovrp_SetEyeTextureArrayEnabled",
    // Pushed by the C# side despite GetDepthCompositingSupported=0; recorded
    // state, like the other setters.
    "ovrp_SetDepthProjInfo",
    // Color-space hints from the C# side; recorded state, like the setters.
    "ovrp_SetClientColorDesc",
    // Audio device ids — PC-legacy queries; NULL until the guest proves it
    // dereferences the answer. The ...2 forms are scalar int returns (the real
    // 0x16db20 NULL-checks its first arg and clamps a negative getter result
    // to 0), so answering 0 is exactly what they do when no Android audio
    // device exists — and our output is CoreAudio, not an Android device.
    "ovrp_GetAudioOutId", "ovrp_GetAudioInId", "ovrp_GetDisplayAdapterId",
    "ovrp_GetAudioOutId2", "ovrp_GetAudioInId2", "ovrp_GetDisplayAdapterId2",
    // Managed-side Media facade init + MRC configuration; ovrpResult/void.
    "ovrp_Media_Initialize", "ovrp_Media_SetMrcAudioSampleRate",
    "ovrp_Media_SetMrcInputVideoBufferType", "ovrp_Media_GetMrcInputVideoBufferType",
    "ovrp_Media_SetMrcActivationMode",
    // The display-object / distortion-window lifecycle. These sat in
    // g_ovrp_bool_yes until 1.40, under the reasoning that libunity's legacy
    // VRDevice ignores the return and 1 is consistent with the other
    // "it worked" answers. Ignored is not the same as unread, and **1.40 reads
    // it**: `OculusDisplayProvider::CreateMobileDisplayObjects` does
    // `cbnz w0 -> "Failed Oculus context setup: %d"` on the result of
    // ovrp_SetupDisplayObjects2 and returns failure, `GfxThread_Start` bails on
    // that, and Unity answers by stopping the display subsystem — which is why
    // the whole 1.40 XR path was one GfxThread_Start immediately followed by
    // GfxThread_Stop, with nothing in between and no error anywhere.
    // **Trap 10, in the same subsystem, six months later.**
    //
    // The convention is not a guess: the real libOVRPlugin.so in this APK ends
    // every one of these with `cmp w0, #0 / csel w0, w0, wzr, lt` and returns
    // -1001/-1002 for bad-parameter / not-initialized, i.e. plain ovrpResult
    // where success is 0 and only negatives are failures. Read it there rather
    // than inferring it from what a caller does with it — a caller that ignores
    // the value cannot tell you which value means yes.
    //
    // Un-suffixed and numbered forms are both listed rather than matched by
    // prefix, for the reason trap 10 exists: a numbered suffix marks an ABI
    // revision, and returning something different under a familiar name is
    // exactly the failure being fixed here.
    "ovrp_SetupDistortionWindow", "ovrp_SetupDistortionWindow3",
    "ovrp_SetupDisplayObjects", "ovrp_SetupDisplayObjects2",
    "ovrp_DestroyDistortionWindow", "ovrp_DestroyDistortionWindow2",
};

static const char *const g_ovrp_bool_yes[] = {
    // bool-returning setters. libunity's OculusVRDevice::Initialize (in the
    // guest, at 0x9bb1fc/0x9bb220/0x9bb2e8) requires each of these to return
    // 1 and deletes the VR device otherwise — answering 0 here is how the run
    // lost the device silently after ovrp_Initialize5. Success is the honest
    // answer: the scale/flip is recorded state in the real plugin, and
    // refusing it would only desync Unity from what we report elsewhere.
    "ovrp_SetEyeTextureScale", "ovrp_SetEyeViewportScale",
    "ovrp_SetEyeTextureFlippedY",
    // The managed side's desired-format push, and the same class: recorded
    // state, and a bool rather than an ovrpResult — the real 1.59 body
    // (+0x2837c) ends `blr <backend>; and w0, w0, #0x1`, and answers 0 only
    // when the plugin is uninitialised. Recorded, not applied: what the eye
    // textures ARE is what ovrp_GetDesiredEyeTextureFormat already answers and
    // what kl_glfb allocates, so accepting the push keeps the guest's story and
    // ours the same. SUPERHOT pushes it during OVRManager's init.
    "ovrp_SetDesiredEyeTextureFormat",
    // Per-frame predicates (0x9bbe58 gate; focus gates whether Unity renders
    // at all). The head is present and the app is focused: true.
    "ovrp_GetNodePresent", "ovrp_GetAppHasVrFocus", "ovrp_GetUserPresent",
    // The OVRP_0_1_2 shape of the tracked predicates: these return ovrpBool
    // DIRECTLY, where the ...Tracked2/...Valid forms above take a u32
    // out-param and return ovrpResult — same question, two ABIs, and only the
    // out-param pair was answered. Managed OVRPlugin reaches for these from
    // OVRInput.GetControllerPositionTracked, so a controller-tracking query
    // from the game's own C# (not libunity's node loop) landed on the
    // unimplemented trampoline and aborted the run.
    "ovrp_GetNodePositionTracked", "ovrp_GetNodeOrientationTracked",
    // ovrp_GetInitialized is NOT here any more — it is STATE, and it moved to a
    // real implementation for the reason klovrp_GetInitialized records. (Nor is
    // ovrp_GetAppHasInputFocus, for a different reason: that one is ovrpResult +
    // out-param, see klovrp_GetAppHasInputFocus below.)
    // Agrees with ovrp_Media_Initialize's success above.
    "ovrp_Media_GetInitialized",
    // The tracking-capability block, which OVRManager reads while it builds a
    // camera rig and SUPERHOT reaches during its main scene's init. Every one of
    // these is a bool in the real 1.59 plugin, by the two v1 wrapper shapes:
    // the getters take no argument and return the ...2 form's value, the setters
    // return `!(result < 0)`. YES is also what is true here rather than merely
    // convenient — we pose the head with full position AND orientation every
    // frame, so reporting either as unsupported or switched off would contradict
    // the poses we hand over on the next call. The setters accept for the same
    // reason the eye-texture ones do: recorded state, with nothing here to
    // switch off.
    "ovrp_GetTrackingPositionSupported", "ovrp_GetTrackingOrientationSupported",
    "ovrp_GetTrackingPositionEnabled",   "ovrp_GetTrackingOrientationEnabled",
    "ovrp_GetTrackingIPDEnabled",
    "ovrp_SetTrackingPositionEnabled",   "ovrp_SetTrackingOrientationEnabled",
    "ovrp_SetTrackingIPDEnabled",
    // Recorded state, like the setters above: there is no boundary here to show
    // or hide, but refusing the call would desync the guest from the coherent
    // "no guardian" story the getters tell.
    "ovrp_SetBoundaryVisible",
    // ovrp_Shutdown and the chromatic-correction pair have moved to real
    // implementations too — all three are `GetInitialized()` in the real
    // library, which is now a variable rather than a constant here.
};

static const char *const g_ovrp_bool_no[] = {
    // Unity asks OVRPlugin which rendering-extension hooks it wants (before/after
    // rendering events, etc.). Our replacement has no render-thread bookkeeping,
    // so "no" is the truthful answer — Unity then never issues the events.
    "UnityRenderingExtQuery",
    // Events that must never fire on a healthy run (0x9bbdf8 acts on
    // recenter; quit/recreate tear things down).
    "ovrp_GetAppShouldRecenter", "ovrp_GetAppShouldQuit",
    "ovrp_GetAppShouldRecreateDistortionWindow",
    // No preview-rect override (return 0 = skip, 0x9bcf9c).
    // ovrp_GetEyeTextureArraySupported / ovrp_GetSystemMultiViewSupported used
    // to live here as a flat no. They are answers now, not constants — see
    // kl_ovrp_multiview() — because on the Vulkan path the honest answer can be
    // yes, and a constant here would have contradicted the 2-forms.
    "ovrp_GetEyePreviewRect",
    // No Guardian here — bool return (real plugin maps failure to false).
    "ovrp_GetBoundaryGeometry2",
    // ...and the v1 predicate beside it, which SUPERHOT's OVRManager asks while
    // it builds its camera rig. Also a bool, by the v1 wrapper shape rather than
    // by its name: the real 1.59 (+0x27b60) calls ovrp_GetBoundaryConfigured2
    // with a stack out-param and answers `csel w0, wzr, w8, lt` — the value if
    // the Result was non-negative, false otherwise. NO is the coherent answer:
    // it agrees with the zero boundary dimensions and the geometry answer above,
    // and this host has no guardian to describe. A title that gated play on a
    // configured boundary would need this revisited — SUPERHOT does not.
    "ovrp_GetBoundaryConfigured",
    // ...and whether one is currently being SHOWN. Nothing shows it here, which
    // is the same answer for the same reason.
    "ovrp_GetBoundaryVisible",
    // Monoscopic rendering: recorded state in the real plugin that starts
    // CLEARED, and we render both eyes for real — so "no" is what is true as
    // well as what an untouched plugin would say. OVRManager reads it while
    // building the camera rig.
    "ovrp_GetAppMonoscopic",
    // No occlusion mesh data exists on this host (bool return).
    "ovrp_GetEyeOcclusionMesh",
    // No fixed-foveated/tiled multires rendering in our GL gateway.
    "ovrp_GetTiledMultiResSupported",
};

static void *klovrp_shared(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_result_ok / sizeof g_ovrp_result_ok[0]; i++)
        if (strcmp(g_ovrp_result_ok[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_ok);
    for (size_t i = 0; i < sizeof g_ovrp_bool_yes / sizeof g_ovrp_bool_yes[0]; i++)
        if (g_ovrp_bool_yes[i] && strcmp(g_ovrp_bool_yes[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_yes);
    for (size_t i = 0; i < sizeof g_ovrp_bool_no / sizeof g_ovrp_bool_no[0]; i++)
        if (g_ovrp_bool_no[i] && strcmp(g_ovrp_bool_no[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_no);
    return NULL;
}

void *kl_ovrp_sym(const char *name) {
    if (!name) return NULL;
    ovrp_slot(name);          // before the impl check, so "resolved" counts everything
    void *fn = kl_ovrp_sym_inner(name);
    if (fn && g_nsym < KL_OVRP_MAX) {
        pthread_mutex_lock(&g_ovrp_mu);
        unsigned i = 0;
        for (; i < g_nsym; i++) if (g_sym[i].ptr == fn) break;
        if (i == g_nsym && g_nsym < KL_OVRP_MAX) {
            g_sym[g_nsym].name = strdup(name);
            g_sym[g_nsym].ptr = fn;
            g_nsym++;
        }
        pthread_mutex_unlock(&g_ovrp_mu);
    }
    return fn;
}

static void *kl_ovrp_sym_inner(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_impl / sizeof g_ovrp_impl[0]; i++)
        if (strcmp(g_ovrp_impl[i].name, name) == 0) return g_ovrp_impl[i].fn;
    void *shared = klovrp_shared(name);
    if (shared) return shared;
    return kl_named_stub(name, (void *)klovrp_called);
}

void kl_ovrp_report(FILE *f) {
    static int done;
    if (done || !g_novrp) return;
    done = 1;
    unsigned called = 0;
    for (unsigned i = 0; i < g_novrp; i++) if (g_ovrp[i].calls) called++;
    fprintf(f, "\n=== OVRPlugin surface (M6 work list) ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_novrp, called);
    // Haptics, both halves in one line per hand: what the guest queued and what
    // a frontend took. Pushes with no pulses is a headless or hand-tracked run
    // and is fine; ZERO pushes on a run that cut notes means the guest never
    // got past the descriptor, which is the failure this line exists to name.
    for (int hand = 0; hand < 2; hand++) {
        pthread_mutex_lock(&g_hap[hand].mu);
        uint64_t pushes = g_hap[hand].pushes, samples = g_hap[hand].samples;
        uint64_t pulses = g_hap[hand].pulses;
        float hpeak = g_hap[hand].peak;
        pthread_mutex_unlock(&g_hap[hand].mu);
        if (!pushes && !pulses) continue;
        fprintf(f, "  haptics %s: %llu buffer(s) / %llu samples queued "
                   "(%.1f s at %d Hz), peak %.2f, %llu pulse(s) played\n",
                hand ? "right" : "left",
                (unsigned long long)pushes, (unsigned long long)samples,
                (double)samples / KLOVRP_HAP_RATE, KLOVRP_HAP_RATE,
                (double)hpeak, (unsigned long long)pulses);
    }
    // The eye swapchain. Frames should be spread across the stages: all of them
    // on one stage means the guest is not cycling and the compositor is reading
    // the texture the guest is writing (see KLOVRP_STAGES_DEFAULT).
    pthread_mutex_lock(&g_frames.mu);
    uint64_t serial = g_frames.serial, guessed = g_frames.guessed;
    uint64_t unobserved = g_frames.unobserved, multi = g_frames.multi;
    uint64_t cross = g_frames.cross_thread, disagree = g_frames.stage_disagree;
    uint64_t named = g_frames.named, named_dis = g_frames.named_disagree;
    uint64_t filed[KLOVRP_MAX_STAGES];
    memcpy(filed, g_frames.filed, sizeof filed);
    pthread_mutex_unlock(&g_frames.mu);
    if (serial) {
        fprintf(f, "  eye swapchain: %d stage(s), frames per stage",
                kl_ovrp_stage_count());
        for (int i = 0; i < kl_ovrp_stage_count(); i++)
            fprintf(f, " %llu", (unsigned long long)filed[i]);
        fprintf(f, " (%llu begun, %llu filed on a guessed stage, "
                   "%llu where the guest's frame index disagreed)\n",
                (unsigned long long)serial, (unsigned long long)guessed,
                (unsigned long long)disagree);
        // The submit-named stage, and whether it was worth reading. On a Vulkan
        // guest this is the ONLY thing that can say which stage holds a
        // finished picture — the GL observation sees nothing there — so a large
        // disagreement here is a compositor that was reading the stage the
        // guest is drawing into next.
        if (named)
            fprintf(f, "  ...of which %llu took the stage the guest NAMED in its "
                       "submit, %llu of them differing from the frame counter%s\n",
                    (unsigned long long)named, (unsigned long long)named_dis,
                    named_dis ? "  <-- the counter would have been wrong there" : "");
        // What the GL side saw, independently of any of the bookkeeping above.
        // A stage whose draw count stops climbing is a frozen picture, and that
        // is a different bug from a mis-filed pose even though both look like
        // doubling in the headset.
        fprintf(f, "  eye draw targets observed per stage:");
        for (int i = 0; i < kl_ovrp_stage_count(); i++)
            fprintf(f, " %llu", (unsigned long long)kl_glfb_stage_draw_count(i));
        fprintf(f, "\n");
        // The association's own health. All three should be 0; each non-zero
        // one names a different reason the pose filed against a stage may not
        // be the pose its picture was drawn with.
        fprintf(f, "  pose<->picture association: %llu frame(s) drew into no eye "
                   "stage, %llu into several, %llu drawn off-thread%s\n",
                (unsigned long long)unobserved, (unsigned long long)multi,
                (unsigned long long)cross,
                (unobserved || multi || cross) ? "" : "  (clean)");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (g_ovrp[i].calls) fprintf(f, "    %-44s x%u\n", g_ovrp[i].name, g_ovrp[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (!g_ovrp[i].calls) fprintf(f, "    %s\n", g_ovrp[i].name);
}
