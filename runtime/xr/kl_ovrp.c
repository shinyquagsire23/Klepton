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
#include <mach/mach.h>   // vm_read_overwrite — probe a guest pointer without faulting
#include "klepton.h"
#include "kl_ovrp.h"
#include "../guest/kl_driver.h"
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
// name. Getting this wrong invents a silent zero that reads as a legitimate
// negative answer several layers from where it came from.
#define OVRP_SUCCESS 0
#define OVRP_TRUE    1
// ...and the two failures the real plugin's own parameter checking uses, both
// read straight off superhot's libOVRPlugin.so: -1001 ovrpFailure_InvalidParameter
// (0x2346c, the NULL-out-pointer arm of ovrp_CalculateLayerDesc) and -1000
// ovrpFailure (0x29a94, the arm ovrp_EnqueueSetupLayer takes when the backend
// will not take the layer).
#define OVRP_FAILURE              (-1000)
#define OVRP_FAILURE_INVALID_PARAM (-1001)

// The failure half of ovrpResult, by name. Every value here is one the real
// libOVRPlugin.so in this APK returns, counted in its disassembly (-1000 x90,
// -1001 x279, -1002 x296, -1003 x126, -1004 x70, and on down) — transcribed,
// not chosen. Naming them matters because the family below is a chain of
// ovrpResult returns, where answering one of these with the wrong sign reads as
// success.
#define OVRP_FAIL_INVALID_PARAM   (-1001)
#define OVRP_FAIL_NOT_INITIALIZED (-1002)
// ...and its mirror: "you already did this". Read in THIS APK's libOVRPlugin.so
// at +0x22800 (ovrp_PreInitialize4's already-preinitialized arm) and +0x229e8
// (ovrp_Initialize6's already-has-a-session arm), both `mov w0, #-0x3eb`.
#define OVRP_FAIL_ALREADY_INITIALIZED (-1003)
#define OVRP_FAIL_UNSUPPORTED     (-1004)
// ...and the one the real plugin uses for "there is nothing to hand you",
// which is not an error condition at the call site — ovrp_PollEvent2's own
// empty-queue path returns it (+0x26750), and the managed event loop reads any
// negative result as "stop polling this frame".
#define OVRP_FAIL_OPERATION       (-1006)

// Record a call. The hand-written implementations below call this themselves so
// that they stay in the report: an entry point silently dropping off it once
// implemented is how the list stops matching what the guest actually does.
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
// KL_OVRP_VERBOSE=1: log every OVRPlugin call's first 20 hits plus a periodic
// heartbeat (every 120th) — comprehensive XR-side tracing (frame loop, app-state
// polls, layer lifecycle, input) in ONE build, toggled per-run by the env var so
// no rebuild is needed to expose or silence it. =2 also drops the periodic cap.
static int kl_ovrp_verbose(void) {
    static int v = -1;
    if (v < 0) v = (int)kl_env_uint("KL_OVRP_VERBOSE", 0);
    return v;
}
static void ovrp_hit(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    if (kl_ovrp_verbose() && s >= 0) {
        unsigned c = g_ovrp[s].calls;
        if (c <= 20 || kl_ovrp_verbose() >= 2 || (c % 120) == 0)
            fprintf(stderr, "  [ovrp-v] %s #%u\n", name, c);
    }
    // -Wframe-address fires on any nonzero level. It is warning about exactly the
    // assumption stated above, which the noinline and the no-tail-call rule are
    // what make good; the fault reporter walks the same chain for the same reason.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-address"
    ovrp_trace(name, __builtin_return_address(1));
#pragma clang diagnostic pop
}

// Log each distinct argument value an input-family entry point is called with
// (node id, controller mask), once each, with the caller's image — names which
// node/mask values the guest actually uses, and whether the poller is libunity
// or the game itself.
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

// Returns ovrpResult: -1004 UNSUPPORTED. For whole subsystems that genuinely do
// not exist on this host (Insight Passthrough has no compositor path wired yet),
// this is the honest answer and it prunes the entire follow-on subtree — the
// guest sees init fail and never calls CreatePassthroughLayer et al.
static uint64_t klovrp_unsupported(const char *name);   // defined with its list, below

// ---------------------------------------------------------------------------
// Has the guest initialised the plugin yet?
//
// This is STATE, not a capability: the one predicate in this file
// whose right answer changes during a run. A constant yes agrees with the state
// only for a guest that calls Initialize first, which Unity does — measured:
// libOculusXRPlugin issues ovrp_Initialize7 seventh and ovrp_GetInitialized
// twelfth.
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
// returns straight out.) A constant yes therefore says the HMD is already up,
// InitDevice returns success having built nothing, ovrp_Initialize5 is never
// called, and the engine goes on to ask that HMD for its eye render target —
// where `FOculusHMD::AllocateRenderTargetTexture` reads
// `EyeLayer_RenderThread->GetSwapChain()` with no null check and takes a
// SIGSEGV at 0x270 on a guest worker thread, four function calls and one
// thread from the answer that caused it.
//
// Set by Initialize5/7, cleared by the Shutdown pair — what the real plugin
// reports.
static int g_ovrp_initialized;

static uint64_t klovrp_GetInitialized(void) {
    ovrp_hit("ovrp_GetInitialized");
    return g_ovrp_initialized ? OVRP_TRUE : 0;
}

// Bring-up, answering SUCCESS by decision rather than reporting a failure the
// guest would be right to believe. Both numbered forms exist because a numbered
// variant can answer something different under a familiar name: Initialize7 is
// the ABI revision 1.40's libOculusXRPlugin calls (it resolves 5, 6 and 7 and
// uses the highest by construction), and RE4's UE4 plugin calls 5. Same answer
// either way; recording it is what lets ovrp_GetInitialized tell the truth.
static uint64_t klovrp_Initialize(const char *name) {
    ovrp_hit(name);
    g_ovrp_initialized = 1;
    return OVRP_SUCCESS;
}
// Audio/display device-id getters, the `ovrpResult f(void **out)` shape.
// ieytd2's libOculusXRPlugin (OculusSystem::Initialize, guest va 0xd2c0/0xd2f4)
// calls each with a ZEROED stack slot as out, checks only the result sign, then
// copies 16 bytes from *out — so success MUST come with a pointer to at least
// 16 readable bytes. A zeroed buffer is the truthful answer ("no Android audio
// device — output is CoreAudio"): it reads as an empty wide string / null GUID.
static uint64_t klovrp_GetDeviceId2(const char *name, const void **out) {
    ovrp_hit(name);
    static const uint8_t empty_id[32];
    if (out) *out = empty_id;
    return OVRP_SUCCESS;
}
// A recenter REQUEST from the guest (menu button long-press UX). On visionOS
// the system owns recentering (crown long-press); acknowledging with success
// and doing nothing is what the guest's UX expects from a platform that
// recenters elsewhere — the recenter-count getter above never increments, so
// the guest sees "requested, nothing changed", which is also what a real Quest
// answers when recentering is deferred. ieytd2 calls it during session setup.
static uint64_t klovrp_RecenterTrackingOrigin2(int flags) {
    (void)flags;
    ovrp_hit("ovrp_RecenterTrackingOrigin2");
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetAudioOutId2(const void **out)      { return klovrp_GetDeviceId2("ovrp_GetAudioOutId2", out); }
static uint64_t klovrp_GetAudioInId2(const void **out)       { return klovrp_GetDeviceId2("ovrp_GetAudioInId2", out); }

// The display adapter id is NOT an audio device — it is the GPU, and a Vulkan
// guest USES it. Unity's OculusXR (acnexusvr, a Unity+Vulkan title) asks here
// which physical device is the VR one, then walks vkEnumeratePhysicalDevices
// matching each device's VkPhysicalDeviceIDProperties.deviceLUID against this
// answer and hands the MATCH to vkGetPhysicalDeviceQueueFamilyProperties. The
// old zeroed answer (via GetDeviceId2) matched nothing once MoltenVK 1.4.2 began
// reporting a real, non-zero LUID, so Unity selected a NULL VkPhysicalDevice and
// crashed in MVKPhysicalDevice::getQueueFamilies. Report the real LUID (first 8
// bytes of the 32-byte id) so the match lands. A GLES guest has no Vulkan
// instance; kl_vulkan_display_luid leaves the id empty, the prior behaviour.
static uint64_t klovrp_GetDisplayAdapterId2(const void **out) {
    ovrp_hit("ovrp_GetDisplayAdapterId2");
    static uint8_t id[32];
    memset(id, 0, sizeof id);
    if (kl_vulkan_display_luid(id))
        fprintf(stderr, "  [ovrp] display adapter LUID %02x%02x%02x%02x%02x%02x%02x%02x\n",
                id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    if (out) *out = id;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Initialize5(void) { return klovrp_Initialize("ovrp_Initialize5"); }
static uint64_t klovrp_Initialize7(void) { return klovrp_Initialize("ovrp_Initialize7"); }

// Deadpool VR's UE4 OculusHMD calls ovrp_PreInitialize5 directly (its sibling
// PreInitialize3, below in the generic list, is the relay Unity's plugin
// resolves). Measured against THIS guest: the four arguments arrive as small
// integers (0, 0xf03, ...) — they are not out-pointers, and a 1.40-inspired
// "zero three out-args" body faults writing to address 0x7. So the answer is
// plain ovrpResult 0, the same bring-up success klovrp_ok stands for, kept
// separate only so the call is visible in the surface report by name.
static uint64_t klovrp_PreInitialize5(void) {
    ovrp_hit("ovrp_PreInitialize5");
    return OVRP_SUCCESS;
}

// ...and the other end: the guest saying the plugin is down. ovrp_Shutdown is
// the ovrpBool wrapper (`!(result < 0)`), so it answers yes; clearing the flag
// is the load-bearing half, since a GetInitialized still saying yes afterwards
// would send a guest that re-enters InitDevice straight back into the fault
// above. ovrp_Shutdown2, the ovrpResult form, was deliberately NOT implemented
// here — "nothing has ever called it, and an abort-by-name is how this project
// finds out that something does". Tetris Effect (UE4.26) is what called it; it
// now has its own handler below, read out of that title's own plugin.
static uint64_t klovrp_Shutdown(void) {
    ovrp_hit("ovrp_Shutdown");
    g_ovrp_initialized = 0;
    return OVRP_TRUE;
}

// ---------------------------------------------------------------------------
// The 4.26 numbered siblings — Tetris Effect's OculusHMD (UE4.26.2,
// ++UE4+Partner-Oculus-4.26) calls PreInitialize4 / Initialize6 / Shutdown2
// where 1.40's Unity provider calls PreInitialize3 / Initialize7 / Shutdown.
//
// **Every shape below is read out of Tetris Effect's own libOVRPlugin.so**,
// not from a header and not from the
// numbered sibling already served above — read the ABI out of the game's own
// binaries. The addresses are that file's.
//
// ovrp_PreInitialize4 (+0x22800): takes no argument it reads — the body touches
// none of x0..x7 before its first call, so whatever arity the real prototype has,
// its inputs are dead on this path. It is an ovrpResult:
//
//     ldrb w8, [x19, #0xd88]   // the "already preinitialized" byte
//     cbz  w8, +0x28           // not yet -> do the work, fall out with w0 = 0
//     mov  w0, #-0x3eb         // already -> -1003 and return
//
// So the FIRST call is 0 and every later one is -1003, and that is transcribed
// rather than flattened to a constant success: a guest that re-enters preinit is
// something we want to see in the log, not something to paper over.
static int g_ovrp_preinitialized;

static uint64_t klovrp_PreInitialize4(void) {
    ovrp_hit("ovrp_PreInitialize4");
    if (g_ovrp_preinitialized) {
        fprintf(stderr, "  [ovrp] ovrp_PreInitialize4 called again -> -1003 "
                        "(already preinitialized, which is what the real plugin "
                        "answers here)\n");
        return (uint64_t)(int64_t)OVRP_FAIL_ALREADY_INITIALIZED;
    }
    g_ovrp_preinitialized = 1;
    return OVRP_SUCCESS;
}

// ovrp_Initialize6 (+0x2299c): ELEVEN arguments — w0 plus x1..x7 in registers
// and three more off the caller's frame (`ldr w10, [x29,#0x10]`,
// `ldr w9, [x29,#0x18]`, `ldr x8, [x29,#0x20]`) — and NOT ONE of them is an
// out-parameter. The session pointer the real body produces is written to its own
// singleton at 0xa0d80 (`ldr x0, [sp,#0x28]; str x0, [x27,#0xd80]`), a slot
// inside the plugin, so there is nothing here for us to fill in on the guest's
// behalf and reading none of the arguments is ABI-safe (AAPCS64: the caller
// passes and cleans up).
//
// It is an ovrpResult, and it carries the preinit work too: if the preinit byte
// at 0xa0d88 is clear it does that first, so a guest may reach Initialize6
// without a PreInitialize4 and still be preinitialized afterwards. That is
// modelled — the flag is set here as well.
//
// -1003 when a session already exists (`ldr x8, [x27,#0xd80]; cbz x8, ...`),
// which is g_ovrp_initialized here. Recording the success is the load-bearing
// half, exactly as for Initialize5/7: ovrp_GetInitialized is the gate UE4's
// FOculusHMD::InitDevice reads, and a wrong answer there is the null-swapchain
// SIGSEGV documented above klovrp_GetInitialized.
static uint64_t klovrp_Initialize6(int api, void *a1, void *a2, void *a3,
                                   void *a4, void *a5, void *a6, void *a7,
                                   int s0, int s1, void *s2) {
    ovrp_hit("ovrp_Initialize6");
    (void)api; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)a7; (void)s0; (void)s1; (void)s2;
    if (g_ovrp_initialized) {
        fprintf(stderr, "  [ovrp] ovrp_Initialize6 with a session already up -> "
                        "-1003\n");
        return (uint64_t)(int64_t)OVRP_FAIL_ALREADY_INITIALIZED;
    }
    g_ovrp_preinitialized = 1;
    g_ovrp_initialized = 1;
    return OVRP_SUCCESS;
}

// ovrp_Shutdown2 (+0x22af0): no arguments, ovrpResult. It is the ovrpResult form
// of the ovrpBool ovrp_Shutdown above and it is NOT the same answer:
//
//     ldr x0, [x8, #0xd80]     // the session singleton
//     cbz x0, +0x30            // none -> -1002, and it does nothing
//     str xzr, [x8, #0xd80]    // clear it, then virtual-destruct it
//     mov w0, wzr              // 0
//
// The comment above klovrp_Shutdown said this entry point was deliberately left
// to abort by name until something called it. Tetris Effect is that something —
// it calls Shutdown2 the moment its eye layer fails to produce a texture.
static uint64_t klovrp_Shutdown2(void) {
    ovrp_hit("ovrp_Shutdown2");
    if (!g_ovrp_initialized) return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
    g_ovrp_initialized = 0;
    return OVRP_SUCCESS;
}

// ovrp_SetEyeFovPremultipliedAlphaMode (+0x230fc): ONE argument, an ovrpBool in
// w0 that the body immediately renormalises (`cmp w0, #0; cset w1, ne`) before
// passing it on; ovrpResult out, -1002 with no session. Whether the eye layer's
// colour is already multiplied by its alpha — a compositor blend mode, and this
// host's compositor takes the eye texture as opaque, so there is nothing behind
// the switch. It is recorded rather than dropped because its getter must agree.
static int g_ovrp_premul_alpha = 1;   // the plugin's own default

static uint64_t klovrp_SetEyeFovPremultipliedAlphaMode(int enabled) {
    ovrp_hit("ovrp_SetEyeFovPremultipliedAlphaMode");
    if (!g_ovrp_initialized) return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
    g_ovrp_premul_alpha = enabled ? 1 : 0;
    return OVRP_SUCCESS;
}

// ...and the getter (+0x23134), which has an OUT-PARAMETER and so cannot be
// served by any shared handler: `cbz x0 -> -1001`, then it stores a 32-bit
// `w8 & 1` through x0. Not called by this guest yet; implemented beside its
// setter so the pair cannot disagree if it ever is.
static uint64_t klovrp_GetEyeFovPremultipliedAlphaMode(int *out) {
    ovrp_hit("ovrp_GetEyeFovPremultipliedAlphaMode");
    if (!out) return (uint64_t)(int64_t)OVRP_FAIL_INVALID_PARAM;
    if (!g_ovrp_initialized) return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
    *out = g_ovrp_premul_alpha;
    return OVRP_SUCCESS;
}

// Who is driving. UE4's OculusHMD calls this immediately after Initialize5 with
// "UnrealEngine" and the engine version — printed once because a version string
// read off the running guest is a fact no unpacked tree states in one place.
//
// Recorded, not applied: nothing on this host behaves differently for one engine
// than another. The shape is read out of RE4's own libOVRPlugin.so (+0x2fb7c)
// rather than inferred: ovrpResult, -1001 if EITHER string is NULL (two `cbz`es
// against a pre-loaded w19), -1002 with no singleton, and the third argument an
// ovrpBool the body passes straight through. The unsuffixed sibling at +0x36990
// is the usual ovrpBool wrapper (`lsr w8, w0, #31; eor w0, w8, #1`), a different
// return convention under a near-identical name, so it gets its own handler.
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
// is what the caller reads, so a scalar-return implementation of these leaves
// the caller doing a `tbnz` on whatever was in its stack slot.
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
// The un-suffixed form returns the level DIRECTLY (the ...2 shape below writes
// it through an out-param and returns ovrpResult). Liminal's Unity 2019 OVRPlugin
// calls this one from its quality/settings pass after the loading intro; without
// it the run aborted mid-render on the unimplemented entry point. Same answer as
// the ...2 sibling — KL_OVRP_MSAA, default 4.
static int klovrp_GetSystemRecommendedMSAALevel(void) {
    ovrp_hit("ovrp_GetSystemRecommendedMSAALevel");
    return (int)kl_env_int("KL_OVRP_MSAA", 4);
}

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
// float without checking the result is a bug no real device would ever expose.
// Zero is what such a caller gets instead of whatever was in its stack slot; a
// caller that checks never sees it.
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
// +0x29f4c is `bl ovrp_GetInitialized; cmp w0, #0; cset w0, ne; ret`). These
// return ovrpBool, so answering ovrpResult's 0 here would read as false.
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
// ovrp_SetColorScaleAndOffset — the compositor fade
// ---------------------------------------------------------------------------
// What it is: OVRManager's global fade. `OVRManager.SetColorScaleAndOffset` is
// what OVRScreenFade and the scene-entry fade-from-black drive — a per-frame
// multiply/add the COMPOSITOR applies to the submitted layers, so the fade
// costs the game nothing and keeps working even if the app misses frames. It
// is the last thing OVRManager does before real content is on screen, which is
// why Batman lands here right after entitlement and the locale queries.
//
// **The signature is read out of the real 1.60 libOVRPlugin.so in this APK,
// not guessed** (Batman: Arkham Shadow's libOVRPlugin.so,
// ovrp_SetColorScaleAndOffset at +0x1c8290, twelve instructions whole):
//
//     adrp x8, 0x522000 ; ldr x8, [x8, #0x280]   ; the backend pointer
//     cbz  x8, +0x30                             ; -> mov w0, #-0x3ea ; ret
//     cmp  w0, #1                                ; applyToAllLayers, in w0
//     mov  x0, x8
//     cset w1, eq                                ; normalised to a strict 0/1
//     bl   <backend>
//     and  w0, w0, w0, asr #31                   ; ovrpResult clamp
//     ret
//
// Three things fall straight out of that. **(1) The integer argument is w0 and
// there is exactly one of it** — `applyToAllLayers`, an ovrpBool, compared
// against 1 rather than against 0, so anything that is not exactly 1 is false.
// **(2) The two vectors are never touched.** The wrapper does not load them,
// spill them, or take their address; it tail-shapes a call to the backend with
// only (backend, bool) in x0/w1 and lets the vectors ride through untouched.
// That is only possible if they arrive in the FP registers — i.e. two
// `ovrpVector4f` BY VALUE, each a four-float HFA, in s0-s3 and s4-s7. A
// pointer-passing signature would have put them in x1/x2 and the wrapper would
// have had to move them. **(3) The return is ovrpResult**, by the
// `and w0, w0, w0, asr #31` idiom this file already names elsewhere: keep w0
// when it is negative, otherwise zero. Not ovrpBool — so 0 here is success.
//
// The HFA reading is confirmed from the other side by the guest's own
// libOculusXRPlugin.so, whose `SetColorScale` (+0x2c390) is a Unity-XR-side
// twin taking the same vector: it opens `str s0,[sp,#0x1c]; str s1,[sp,#0x18];
// str s2,[sp,#0x14]; str s3,[sp,#0x10]` and copies the four words into its
// static `s_ColorScale`. Four floats, four single registers. (That path is a
// SEPARATE one: the Unity provider stows s_ColorScale/s_ColorOffset and stamps
// them into the layer submit at +0x48/+0x58 with OverridePerLayerColorScaleAndOffset.
// Batman is not using it — it calls the OVRPlugin global directly.)
//
// **RECORDED, NOT APPLIED, and that is a measurement rather than a shrug.**
// There is no colour hook to wire this into: the eye-layer submit's ColorScale
// / ColorOffset fields at +0x48/+0x58 are parsed in ovrp_layer_submit below and
// read by nothing, and the Metal compositor has no per-frame tint stage at all.
// So there is no existing path to route this through, and inventing one is a
// render-side change of its own.
//
// The cost of recording is understood and is the cheap direction: a fade the
// compositor ignores means the game presents at FULL BRIGHTNESS through what
// should have been a fade-from-black — a scene appears instantly instead of
// easing in. Cosmetic. The other direction is not: refusing the call returns a
// negative ovrpResult to managed code that is mid-fade, and OVRScreenFade's
// usual shape is to drive the fade to completion and only then let the scene
// run — a refusal there can leave the guest waiting on a fade it believes never
// started. Accept, record, and stay out of the way.
//
// The values are kept so a compositor that grows a tint stage can apply them
// without re-deriving anything, and the first call is logged with its numbers
// because "the fade ran" and "the fade never started" look identical on a
// screen that never dims.
typedef struct { float x, y, z, w; } ovrp_vector4f;

static ovrp_vector4f g_ovrp_color_scale  = { 1.0f, 1.0f, 1.0f, 1.0f };
static ovrp_vector4f g_ovrp_color_offset = { 0.0f, 0.0f, 0.0f, 0.0f };
static int           g_ovrp_color_all_layers;

static uint64_t klovrp_SetColorScaleAndOffset(ovrp_vector4f scale,
                                              ovrp_vector4f offset,
                                              uint32_t apply_to_all_layers) {
    ovrp_hit("ovrp_SetColorScaleAndOffset");
    g_ovrp_color_scale  = scale;
    g_ovrp_color_offset = offset;
    // The real wrapper's `cmp w0,#1 / cset w1,eq`: exactly 1 is true, and
    // every other bit pattern — including 2, which a marshalled C# bool can be
    // — is false. Copied rather than written as `!= 0` so the recorded state
    // says the same thing the device would have.
    g_ovrp_color_all_layers = (apply_to_all_layers == 1);

    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ovrp] SetColorScaleAndOffset scale=(%.3f %.3f %.3f %.3f) "
                        "offset=(%.3f %.3f %.3f %.3f) allLayers=%d "
                        "-- RECORDED, the compositor has no tint stage; the fade "
                        "is skipped and the picture stays at full brightness\n",
                scale.x, scale.y, scale.z, scale.w,
                offset.x, offset.y, offset.z, offset.w,
                g_ovrp_color_all_layers);
    }
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// The synthetic headset
// Returns ovrpResult: a REFUSAL. The third answer shape, and it exists because
// the first two cannot say this one thing: "the call was understood, the service
// it asks for is not here."
//
// ovrpFailure is -1000. It is not ovrpFailure_NotInitialized (-1002), which is
// what the real plugin returns from this family when nothing has come up at all
// — we ARE initialized and every other surface says so, and a getter that
// contradicts the rest of the runtime is the trap-6d shape this file keeps
// warning about. -1000 is the value ovrp_EnqueueSetupLayer itself produces
// (superhot's libOVRPlugin.so, 0x29a94: `mov w8, #-0x3e8`) when the backend
// declines the layer, which is exactly what is happening.
//
// Logged the first time each name is refused, because a refusal is a fork in the
// guest's behaviour and the log has to show where it was taken.
static uint64_t klovrp_refuse(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) {
        g_ovrp[s].calls++;
        if (g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] %s -> ovrpFailure: this runtime composites the "
                            "eye layer and no other\n", name);
    }
    return (uint64_t)(int64_t)OVRP_FAILURE;
}

// Unity's audio-plugin enumeration, dlsym'd speculatively on every plugin
// handle libunity holds. The real 1.59 libOVRPlugin does NOT export it — the
// spatializer is its own libAudioPluginOculusSpatializer.so, which the guest
// loads and queries separately — so on Android this dlsym fails and Unity
// moves on. Our resolve trampoline makes every name look present, so the
// absence is answered as a function instead: zero effects, out-param
// untouched, the same "nothing here" the failed dlsym concluded.
static uint64_t klovrp_UnityGetAudioEffectDefinitions(void *out) {
    ovrp_hit("UnityGetAudioEffectDefinitions");
    (void)out;
    return 0;
}

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

// The 2-form is not the un-suffixed function under another name.
// Real ovrp_GetVersion2 (+0x16d970) is `ovrpResult ovrp_GetVersion2(const char **)`:
// it `cbz x0` -> -1001, stores the string through `[x0]`, and returns 0. The
// un-suffixed one at +0x175ab0 is a WRAPPER around it — a stack slot, a call,
// and `csel x0, xzr, x8, lt` — so that one, and only that one, returns a
// scalar. Same convention as klovrp_GetSystemProductName2 two hundred lines
// down.
//
// The OUT parameter must be written even though libunity and libOculusXRPlugin
// reach this through the C# `ovrp_GetVersion` and ignore it: 1.40's
// libhaptics_sdk calls the 2-form directly from native Rust and `strlen`s the
// slot. Unwritten, that is a SIGSEGV at 0x0 inside `_platform_strlen` with one
// guest frame above it and nothing naming haptics, the version, or this
// function — reachable only once the PCM entry points stop aborting first.
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
// from the naming. Writing the pair the other way round leaves the 2-form's out
// parameter unwritten, and the caller reads its own uninitialised stack slot.
//
// What it reports is the version of the NATIVE SDK under the plugin — VrApi on
// a Quest. There is none here (libvrapi.so is never loaded), so
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
#define OVRP_HEADSET_OCULUS_QUEST   8
#define OVRP_HEADSET_OCULUS_QUEST_2 9
// KL_OVRP_HEADSET=<n> overrides the reported ovrpSystemHeadset. Default is
// Quest 2 (9). Liminal (Oculus Utilities 1.40, an SDK that predates Quest 2)
// keys its Touch-vs-3DoF-remote controller mapping on headset == Oculus_Quest
// (8): an unknown 9 falls to the remote branch and its hands read as never
// connected. Running it with KL_OVRP_HEADSET=8 presents the headset the app
// knows uses Touch, so hands map to LTouch/RTouch.
static int klovrp_headset_type(void) {
    static int v = -1;
    if (v < 0) {
        // Liminal (Oculus Utilities 1.40) maps hands to Touch only when the
        // headset reports Oculus_Quest (8); a 9 (Quest 2) leaves its hands on
        // the 3DoF-remote branch and they read as never connected. So the
        // liminalvr target defaults to 8 with no env needed; everything else
        // keeps the Quest 2 default. KL_OVRP_HEADSET still overrides either.
        // missioniss (same OVRPlugin-1.40 era) shows the identical connected=0x0
        // symptom, BUT presenting headset==8 makes it engage the Touch input path
        // that then CRASHES on a guest worker thread (signal 11) — an
        // unimplemented Touch-path ovrp call, not yet isolated. So missioniss stays
        // on the Quest-2 default (stuck-at-menu but stable) until that crash is
        // named; test the Touch path on demand with KL_OVRP_HEADSET=8.
        const char *tgt = kl_driver_target_name();
        int dflt = (tgt && strcmp(tgt, "liminalvr") == 0)
                     ? OVRP_HEADSET_OCULUS_QUEST
                     : OVRP_HEADSET_OCULUS_QUEST_2;
        v = (int)kl_env_int("KL_OVRP_HEADSET", dflt);
    }
    return v;
}
static uint64_t klovrp_GetSystemHeadsetType(void) {
    ovrp_hit("ovrp_GetSystemHeadsetType");
    return (uint64_t)(uint32_t)klovrp_headset_type();
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
    *out = klovrp_headset_type();
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

// ---------------------------------------------------------------------------
// The capability sweep, and why every one of it needs its own function.
//
// OVRManager reads a row of predicates while it builds the camera rig — is eye
// / face / body / hand tracking ENABLED, is the foveation gaze-driven, is it
// dynamic, does the panel dim locally, is the headset in 3DoF mode, can the GPU
// be polled. In OVRPlugin.cs every one of them has the SAME shape,
// `ovrpResult f(ovrpBool *)`: a Result return and one out-parameter. And every
// one of them is FALSE here — this runtime presents Touch controllers, a fixed
// rate map and no tracker of any kind behind those names.
//
// `ovrp_GetHandTrackingEnabled` is the one that was FATAL. The Last Clockwinder
// reached the frame pump, composited 1693 frames, and then died on
//
//     [klepton] fatal: guest called unimplemented OVRPlugin entry point
//               'ovrp_GetHandTrackingEnabled', from ?+0x0
//
// — from IL2CPP'd managed code, which is why the caller resolves to nothing.
// The rest of the row is answered WITH it for the reason g_ovrp_layer_no gives
// about layers: a sweep answered one call at a time only moves the abort to the
// right, and costs a session in the headset per step.
//
// These are real table entries and NOT names in g_ovrp_bool_no, which is where
// a first draft put them, and the difference is the out-parameter. The shared
// lists are served through kl_named_stub, which puts the entry point's NAME in
// x0 — so the guest's first argument, the `ovrpBool *`, is gone by the time the
// handler runs. That is exactly why klovrp_ok / _yes / _no / _hand_unsupported
// all read no argument and write no out-parameter. A predicate answered that way
// returns Success and leaves the caller's Bool holding whatever was on its
// stack; today's wrapper happens to initialise it (`Bool val = Bool.False;`),
// which is a property of one version of one C# file and not something to build
// on. A direct entry keeps the pointer, so the zero is really written.
//
// One function per name rather than one shared function, because the surface
// report attributes calls by the name the handler passes to ovrp_hit — sharing
// would file the whole sweep under whichever name was typed, and that report is
// how the next wall gets found.
#define KLOVRP_CAPABILITY_OFF(nm)                                              \
    static uint64_t klovrp_##nm(int32_t *out) {                                \
        ovrp_hit("ovrp_" #nm);                                                 \
        if (out) *out = 0;                                                     \
        return OVRP_SUCCESS;                                                   \
    }

KLOVRP_CAPABILITY_OFF(GetHandTrackingEnabled)
KLOVRP_CAPABILITY_OFF(GetEyeTrackingSupported)
KLOVRP_CAPABILITY_OFF(GetEyeTrackingEnabled)
KLOVRP_CAPABILITY_OFF(GetFaceTrackingSupported)
KLOVRP_CAPABILITY_OFF(GetFaceTrackingEnabled)
KLOVRP_CAPABILITY_OFF(GetBodyTrackingSupported)
KLOVRP_CAPABILITY_OFF(GetBodyTrackingEnabled)
KLOVRP_CAPABILITY_OFF(GetFullBodyTrackingEnabled)
// Eye-tracked foveation, the scalar beside ovrp_GetFoveationEyeTrackedSupported
// above: not supported, therefore not on.
KLOVRP_CAPABILITY_OFF(GetFoveationEyeTracked)
// The dynamic half of the tiled-multires family. ovrp_GetTiledMultiResSupported
// already answers no in g_ovrp_bool_no (it is a bare ovrpBool return, so it
// belongs there); this one carries an out-parameter and belongs here.
KLOVRP_CAPABILITY_OFF(GetTiledMultiResDynamic)
KLOVRP_CAPABILITY_OFF(GetLocalDimmingSupported)
KLOVRP_CAPABILITY_OFF(GetSystemHmd3DofModeEnabled)
KLOVRP_CAPABILITY_OFF(GetGPUUtilSupported)

#undef KLOVRP_CAPABILITY_OFF

// Handedness. `ovrpHandedness` is { Unsupported = 0, LeftHanded = 1,
// RightHanded = 2 }, and OVRManager reads it while it builds the camera rig —
// The Last Clockwinder died on it one call after the capability sweep above.
//
// Unsupported is the wrong answer even though we have no such setting: OVRInput
// maps the dominant hand onto a controller, and Unsupported leaves titles that
// branch on it without one. A Quest ships right-handed and lets the user change
// it, so that is the default here, with a knob for the other answer rather than
// a second guess.
static uint64_t klovrp_GetDominantHand(int32_t *out) {
    ovrp_hit("ovrp_GetDominantHand");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    int left = kl_env_on("KL_OVRP_LEFT_HANDED", 0);
    *out = left ? 1 : 2;
    return OVRP_SUCCESS;
}

// Multiview — "can the two eyes be two ARRAY LAYERS of one texture, rendered in
// one pass?" — is the single answer that decides the guest's whole stereo
// rendering mode, and it is ours to give.
//
// OFF on GL, and that is a property of the gateway rather than of the hardware:
// ANGLE-on-Metal has no multiview, so every GLES guest is told no, and the
// un-suffixed scalar form in g_ovrp_bool_no says the same thing. Yes to a GLES
// guest is an eye texture it renders half of.
//
// ON on the VULKAN path, because MoltenVK implements `VK_KHR_multiview` and
// kl_vulkan.c hands back a two-layer image. Not a performance choice: no puts
// Unity in MultiPass, and BONELAB's SRP throws `ArgumentOutOfRangeException`
// once per frame before drawing anything in that mode — 856 in a 255-frame run,
// each followed by `XRSystem.ReleaseFrame() was not called!` and an empty stack
// trace (the build carries none). Nothing about the XR display is wrong there:
// `XRSettings` reports `enabled=1`, a 2208x2400 eye texture and a healthy
// `oculus display`. A Quest title ships its shader variants and its renderer for
// Single Pass Instanced; MultiPass is a path it may never have run.
//
// The answer is gated on a Vulkan device actually having come up through this
// runtime, so a GLES guest is unaffected. `KL_OVRP_MULTIVIEW=0` is the A/B and
// restores the MultiPass configuration exactly.
//
// The two forms must not disagree: ovrp_GetEyeTextureArraySupported is the same
// question and reads this.
//
// Per-target DEFAULT, mirroring klovrp_unify_default's precedent. olar and
// wanderer are UE5/Vulkan titles that set up a correct multiview render pass —
// a viewType_2D_ARRAY colour view over both eye layers plus a 2-layer D32S8
// depth, targeting the very eye VkImage GetLayerTexture2 hands out — and yet the
// eye comes back GENUINELY BLACK at the guest-queue copy point (KL_VK_OUT: "0
// lit", both eyes, every frame, after a vkQueueWaitIdle). The render executes
// but MoltenVK's VK_KHR_multiview -> Metal translation of the UE5 world shaders
// produces nothing (olar additionally shows torn garbage geometry; its crisp
// menu is a SEPARATE Quad overlay layer the host composites, not eye content).
// Defaulting these off puts the guest in per-eye MultiPass, which does not go
// through the multiview path at all. KL_OVRP_MULTIVIEW overrides in either
// direction, so a rebuild is not needed to A/B it.
// Multiview defaults ON. Turning it off is NOT a usable A/B for olar/wanderer:
// UE5's Meta-XR eye layer is Array-only (it keeps requesting ovrpLayout_Array
// even when we report no multiview), and GetLayerTexture2 then refuses the Array
// layer (multiview off) — olar spun ~86k CalculateEyeLayerDesc retries and the
// watchdog killed it. So these titles MUST have multiview to boot at all; the
// black-world fix has to live inside the multiview path, not by disabling it.
int kl_ovrp_multiview(void) {
    // GLES (ANGLE) guests have NO multiview and their eye layer is served only as
    // Stereo/DoubleWide — so multiview MUST read false for them, or a UE guest
    // (twd2) requests an Array eye layout that GetLayerTexture2 refuses on the GL
    // path, and the guest spins forever recalculating the layer it can never get.
    // Only the Vulkan path serves Array. (Dropping this guard once regressed twd2
    // into exactly that non-booting CalculateLayerDesc loop.)
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

// ovrp_GetTrackingPositionSupported2(ovrpBool* out) — Red Matter 2 asks it;
// positional tracking IS supported. Answered by KLOVRP_OUT_TRUE below.

// The un-suffixed pair. Both are plain `bool` WRAPPERS around the 2-forms in the
// real plugin — `bl ...2; csel w0, wzr, w8, lt`, i.e. the out value when the
// Result was non-negative and false otherwise (read at +0x130bc0 / +0x130b60) —
// so they are the scalar-bool shape and must answer whatever the 2-forms do. A
// constant here beside the 2-forms' variable answer means the guest asks twice
// under two names and acts on whichever it read last.
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

// The tracking supported/enabled sweep a UE4.27 OculusHMD (Deadpool VR) makes
// beside the capability "no"s: ovrpResult + ovrpBool* out, like the 2-forms
// above. 6DoF head tracking exists here (the compositor supplies poses), so
// each answers the Quest's own: position and orientation supported and on, IPD
// tracking on. A list entry in g_ovrp_bool_yes would return 1 and leave the
// out-slot holding the guest's own stack.
#define KLOVRP_OUT_TRUE(fn)                                   \
    static uint64_t klovrp_##fn(int *out) {                   \
        ovrp_hit("ovrp_" #fn);                                \
        if (!out) return (uint64_t)(int64_t)OVRP_FAIL_INVALID_PARAM; \
        *out = 1;                                             \
        return OVRP_SUCCESS;                                  \
    }
KLOVRP_OUT_TRUE(GetTrackingPositionSupported2)
KLOVRP_OUT_TRUE(GetTrackingOrientationSupported2)
KLOVRP_OUT_TRUE(GetTrackingPositionEnabled2)
KLOVRP_OUT_TRUE(GetTrackingOrientationEnabled2)
KLOVRP_OUT_TRUE(GetTrackingIPDEnabled2)
#undef KLOVRP_OUT_TRUE

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
// addresses.

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

// ...and what the PANEL runs at, kept SEPARATELY from what the guest is told.
//
// THESE ARE TWO DIFFERENT LAYERS, and Paint the Town Red VR proved it. The
// guest paces itself off the number we hand it: Unity stores it in its VR
// timing config, and the frame budget, the fixed timestep and the audio
// scheduler are all sized from it. A 72 Hz Quest title told "120.00" is told it
// has 8.33 ms to do 13.9 ms of work. It then misses EVERY deadline while every
// host-side compositor statistic stays perfect — 1.00 periods, 0 past deadline,
// 0 held — because the compositor does not need the guest to agree with the
// panel: it reprojects whatever the guest last submitted, every panel period.
// The visible result is a picture arriving on an uneven grid, which reads as
// jitter under head rotation. Paint the Town Red's log is exactly that: the guest
// asked for its build rate with ovrp_SetSystemDisplayFrequency(72.0), was
// refused because we had told it 120, and then delivered 136 distinct frames
// into 300 panel slots with the cadence line reading a flawless 1.00 periods.
//
// So the honest arrangement is the one a Quest actually presents: the guest is
// told the rate it was BUILT for, and the compositor keeps running at the
// panel's. A PIN is what separates them, and there are two doors to it:
//
//   - KL_DISPLAY_HZ, the run's own override; and
//   - kl_ovrp_set_forced_hz_hint(), a frontend pinning the rate for a guest known
//     to mishandle a measured one. Steam Link's VR client is the case: its
//     QSVLClient::UpdateFrameVariables (+0xf14e0) takes an empty-container
//     branch for the display-rate submission and dereferences its null data
//     pointer regardless, so it must be told a rate up front. The env wins over
//     the hint.
//
// With NO pin, behaviour is exactly what it always was — the measured panel
// rate becomes the guest's.
static float g_display_hz_panel;
static int   g_display_hz_pinned;
static float g_forced_hz_hint;

void kl_ovrp_set_forced_hz_hint(float hz) { g_forced_hz_hint = hz; }

// The pin has to be established before EITHER accessor can act on it, because
// the order they are first called in is a property of the frontend. It used to
// be read lazily in the getter alone, which made KL_DISPLAY_HZ depend on
// whether anything happened to ask before the compositor's priming pass pushed
// its measurement — a getter call in front of that push consumed the env and
// the push then overwrote it. Read once, from both doors, through the same
// 30..240 sanity check as the setter so a typo cannot poison pacing math.
static void display_hz_init(void) {
    static int checked;
    if (checked) return;
    checked = 1;
    const char *e = getenv("KL_DISPLAY_HZ");
    float hz = (e && *e) ? strtof(e, NULL) : 0.0f;
    const char *why = "by KL_DISPLAY_HZ";
    if (!(hz >= 30.0f) || !(hz <= 240.0f)) {
        hz = g_forced_hz_hint;
        why = "for this target";
        if (!(hz >= 30.0f) || !(hz <= 240.0f)) return;
    }
    fprintf(stderr, "  [ovrp] display frequency PINNED to %.1f Hz %s — the guest "
                    "is paced at this rate whatever the panel measures\n",
            (double)hz, why);
    g_display_hz = hz;
    g_display_hz_pinned = 1;
}

void kl_ovrp_set_display_frequency(float hz) {
    // A display frequency is a divisor in Unity's pacing math and a period
    // everywhere else. Anything outside the range a headset can actually run at
    // is a measurement that went wrong, and passing it on turns one bad number
    // into a frame loop that never settles.
    if (!(hz >= 30.0f) || !(hz <= 240.0f)) return;
    display_hz_init();
    g_display_hz_panel = hz;
    if (g_display_hz_pinned) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] the panel measures %.2f Hz; the guest keeps "
                            "its pinned %.1f Hz (the compositor still runs at the "
                            "panel's rate and reprojects)\n",
                    (double)hz, (double)g_display_hz);
        }
        return;
    }
    g_display_hz = hz;
}

// What the GUEST is told. Every guest-facing consumer reads this one:
// ovrp_GetSystemDisplayFrequency{,2}, the available-frequencies list, the
// OpenXR XR_FB_display_refresh_rate trio, the uniform guest frame clock and the
// Choreographer's cadence in kl_jni_looper.c — so the guest cannot be told two
// different rates by two APIs.
//
// KL_DISPLAY_HZ=<hz> pins it. On the host there is no headset to measure, so
// the 72 above is the Quest-2 fiction the rest of the device description keeps
// up; on device the pin is how a title is paced at the rate it was built for,
// whatever the panel measures — see display_hz_init.
float kl_ovrp_display_frequency(void) {
    display_hz_init();
    return g_display_hz;
}

// ...and what the PANEL runs at, for the few host-side consumers that are
// measuring our own presentation rather than describing the device to the
// guest. Falls back to the guest-facing number when nothing has been measured,
// which is every host run.
float kl_ovrp_display_frequency_panel(void) {
    display_hz_init();
    return g_display_hz_panel > 0.0f ? g_display_hz_panel : g_display_hz;
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

// The ceiling on a per-eye render target, KL_OVRP_EYE_MAX, applied in the one
// place so that every producer of an eye size answers to the same number.
//
// It has two callers and it used to have one, which was the whole bug. The
// advertisement (kl_ovrp_set_eye_texture_size, below) was capped; the
// DESC the guest actually allocates from was not, because Unity multiplies the
// advertised size by its own render scale on the way in
// (`XRSettings.eyeTextureResolutionScale`, arriving as ovrp_CalculateEyeLayerDesc3's
// textureScale) and nothing clamped the product. Paint the Town Red VR ships
// 1.5, so a 4032x3234 advertisement became a 6048x4851 desc and a 7584x4851 eye
// layer — 2.25x the pixels, in OUR images and in every render target Unity
// sizes from them. Footprint went 620 MiB -> 7170 MiB in two seconds and jetsam
// took the process three seconds into the first picture the title drew.
// SUPERHOT died of the same shape at 2x (see klovrp_SetupEyeTexture2).
//
// A guest's render scale is SUPERSAMPLING: it was authored as a multiple of the
// eye size its own platform recommends, which on a Quest is a third of what this
// host reports. Honouring it above the cap is not "the resolution the game
// wanted", it is a multiplier applied twice. Clamping it back to the cap is
// therefore not a resolution cut — the cap IS the native size.
//
// KL_OVRP_EYE_ALIGN=<n> (default 1 = off) then rounds each axis DOWN to a
// multiple of n, after the cap, on both callers. Unreal quantizes its scene
// buffers UP to a multiple of 4 (QuantizeSceneBufferSize), so RE4 (4.25.3)
// rendered a 4032x3236 scene into a 4032x3234 eye: anything that matches the
// guest's eye passes by exact extent (kl_vulkan.c's eye-pass recognition) then
// misses every one of them (measured: 0 of 15193 encoders recognised). An eye
// that is already a multiple of 4 is what the guest's own buffers come out at.
void kl_ovrp_cap_eye_size(int *wp, int *hp) {
    if (!wp || !hp) return;
    int w = *wp, h = *hp;
    if (w <= 0 || h <= 0) return;
    int cap = kl_env_int("KL_OVRP_EYE_MAX", 3072);
    if (cap > 0 && (w > cap || h > cap)) {
        // Preserve the aspect ratio: clamping the axes independently would
        // change the shape of the picture, and the frustum tangents pushed
        // alongside this would then disagree with it.
        float k = (float)cap / (float)(w > h ? w : h);
        w = (int)((float)w * k + 0.5f);
        h = (int)((float)h * k + 0.5f);
    }
    int al = kl_env_int("KL_OVRP_EYE_ALIGN", 1);
    if (al > 1 && al <= 64) {
        if (w >= al) w -= w % al;
        if (h >= al) h -= h % al;
    }
    *wp = w > 0 ? w : 1;
    *hp = h > 0 ? h : 1;
}

// KL_OVRP_EYE_SCALE_MIN=<x> (default 0 = off): the FLOOR under a guest's own
// render scale (Unity's XRSettings.eyeTextureResolutionScale, arriving as
// ovrp_CalculateEyeLayerDesc3's textureScale), the counterpart of the ceiling
// above. The floor applies first and the cap second, so a 1.5 menu scale is
// still held to KL_OVRP_EYE_MAX and a floor can never push past it.
//
// Paint the Town Red VR: the game sets a PER-LEVEL render scale
// tuned for a Quest 2 (1.5 menu, 1.0 Disco, 0.87 Prison). Every change makes
// Unity tear down the eye layer and reallocate its eye-scaled targets at the
// new size while the old generation is still alive — measured at the Prison
// load as a 7.84 GB peak with 355 MB free (the hard limit is ~8.19 GB), and
// Prison then rendered at 0.87 of an eye that is already capped below the
// display's ask. With the floor at 1.0 every level asks for the same desc, the
// eye storage is reused (klovrp_SetupLayer "reusing the previous layer's
// textures"), and the lower-scale levels render at the cap. It costs those
// levels GPU: 0.87 -> 1.0 is +32% pixels.
float kl_ovrp_eye_scale_floor(float asked) {
    float lo = kl_env_float("KL_OVRP_EYE_SCALE_MIN", 0.0f);
    if (!(lo > 0.0f) || lo > 4.0f) return asked;
    return asked < lo ? lo : asked;
}

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
    kl_ovrp_cap_eye_size(&w, &h);
    if (w == g_eye_w && h == g_eye_h) return;

    // Logged every change, not once: each one costs the guest a swapchain
    // rebuild, and how many of those happen is the question. The MiB figure is
    // the whole swapchain Unity will hold.
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

// Titles that read the frustum ONCE and render both eyes with that single cone,
// then submit each eye full-width (no per-eye ViewportRect) — the "collapses the
// cones WITHOUT submitting per-eye rects" case KL_OVRP_UNIFY_FRUSTUM exists for.
// On the canted Vision Pro display the composite otherwise reprojects the right
// eye through the right cone while the picture in it was rendered with the left
// cone, and the two eyes shear apart into crossed stereo. Into The Radius reads
// ovrp_GetNodeFrustum2 exactly once and submits eye0/eye1 both at 0,0 full-width
// (measured). Defaulting unify on for it makes the guest render, and the
// composite reproject, one symmetric union cone per eye — they agree, the cross
// is gone, at the known cost of the union's slightly lower angular density. The
// env still overrides in either direction.
static int klovrp_unify_default(void) {
    static const char *on[] = { "intotheradius" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
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
    if (on < 0) on = kl_env_on("KL_OVRP_UNIFY_FRUSTUM", klovrp_unify_default());
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

// How much WIDER than our recommendation the guest's eye texture is going to be.
//
// A canted display's two cones are not the same cone, and an Oculus-style guest
// does not render two different frustums: libOculusXRPlugin (and UE5's OculusXR
// HMD, which is the same code) takes the UNION — max(L0,L1), max(R0,R1) —
// renders both eyes through it, widens its own eye texture by the ratio so the
// angular density it was recommended is preserved, and then submits a per-eye
// ovrpLayerSubmit.ViewportRect naming each eye's sub-rect inside the wider
// image. That is documented above klovrp_eye_tan, it is what BONELAB does, and
// Pinball FX was measured doing exactly it — on the HOST, once
// KL_OVRP_EYE_FORCE reached a UE target:
//
//   desc 3072x2464  ->  SetupLayer 3840x2464
//   render viewport eye0 0,0 3635x2957   eye1 974,0 3636x2957
//
// 974/3635 = 0.2680, which is the same fixed fraction the DEVICE log carries
// across the whole DRS ramp. The widening and the per-eye packing are one
// behaviour, they are correct, and nothing here tries to undo them.
//
// What the ratio is FOR is the cap. KL_OVRP_EYE_MAX bounds the size we
// recommend; the guest then allocates that times this, so a cap of 4032 on a
// canted display bought a 5088-wide texture and read as "the cap does nothing".
// Predicting the widening is what lets the cap bound the allocation instead of
// the recommendation — see klovrp_eye_desc_size.
//
// 1.0 whenever the two cones are the same (every symmetric display, every host
// run without KL_OVRP_EYE_TAN, and any guest we have unified the frustum for
// ourselves — klovrp_eye_tan has already applied KL_OVRP_UNIFY_FRUSTUM by the
// time we read it, and after that there is nothing left for the guest to widen).
static float klovrp_union_widen(void) {
    const float *t0 = klovrp_eye_tan(0);
    const float *t1 = klovrp_eye_tan(1);
    float per = t0[0] + t0[1];
    float uni = (t0[0] > t1[0] ? t0[0] : t1[0]) + (t0[1] > t1[1] ? t0[1] : t1[1]);
    if (!(per > 0.0f) || !(uni > per)) return 1.0f;
    return uni / per;
}

// The head->eye offsets, head-local metres, and the guest's whole source of
// stereo separation — see kl_ovrp.h. Zero is the historical host behaviour and
// stays the default; the visionOS compositor pushes the display's own numbers.
static float g_eye_off[2][3];

// How often the pushed value actually CHANGES, and by how much.
//
// The frontend calls this once per compositor iteration (pushEyeOffsets), i.e.
// at panel rate, and the value is NOT part of the per-frame pose latch — so a
// guest that reads it between its two eye renders could in principle get two
// different eye positions, which is a per-eye error and therefore a doubling
// candidate. Whether that is real or theoretical depends entirely on whether
// the number moves at all: `view.transform.columns.3` is the headset's own
// device_from_view, fixed for the run unless the display re-measures IPD.
// Measure before latching. `changes 0` over a whole session closes the
// question; a nonzero count says the latch is owed.
static uint64_t g_eye_off_pushes, g_eye_off_changes;
static float    g_eye_off_worst_mm;

void kl_ovrp_set_eye_offset(int eye, float x, float y, float z) {
    if ((unsigned)eye > 1) return;
    g_eye_off_pushes++;
    float dx = x - g_eye_off[eye][0];
    float dy = y - g_eye_off[eye][1];
    float dz = z - g_eye_off[eye][2];
    float d = sqrtf(dx*dx + dy*dy + dz*dz) * 1000.0f;
    // Not the first push, which is 0 -> the real offset and always "a change".
    if (g_eye_off_pushes > 2 && d > 0.0f) {
        g_eye_off_changes++;
        if (d > g_eye_off_worst_mm) g_eye_off_worst_mm = d;
    }
    g_eye_off[eye][0] = x; g_eye_off[eye][1] = y; g_eye_off[eye][2] = z;
}

// The shortest angle between two unit quaternions laid out xyzw, in degrees.
// Here rather than beside klovrp_quat_degrees (which is a klovrp_pose wrapper
// over this) because the eye-rotation churn counter below needs it and lives
// above that point in the file.
static float klovrp_quat4_degrees(const float a[4], const float b[4]) {
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * (180.0f / 3.14159265358979f);
}

// The eye's own ROTATION — the cant — which the guest needs along with the
// tangents.
//
// Vision Pro's displays are angled outward, so device_from_view is not a pure
// translation: each eye is turned, oppositely, and its frustum tangents are
// expressed in that turned frame (measured: l=1.7321 r=1.0000 for the left eye,
// mirrored for the right — tan 60 out, tan 45 in). Handing over the turned
// frame's tangents while reporting an eye that points straight ahead renders
// the right shape of cone in the wrong direction; kl_reproject.c then views
// that picture through the real canted eye (view_rot), which is correct for the
// picture it was given, and each eye's image lands rotated by the cant in
// opposite directions. Opposite per-eye rotation is the one error the visual
// system cannot merge — it is seen as TWO IMAGES rather than as blur.
//
// Identity by default, so a host run and every headless test are unchanged.
// KL_OVRP_EYE_CANT=0 drops the cant again, as the A/B.
static float g_eye_rot[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };

// The rotation half of the same churn question the translation counters above
// ask. `eye offset pushes 1864, changes 1843, worst 0.178 mm` said the
// TRANSLATION barely moves — 0.178 mm is nothing — but it said nothing at all
// about the eye's orientation, and an eye transform that turns between the
// guest's two eye renders is a per-eye error no reprojection can undo. Same
// counters, same window, in degrees.
static uint64_t g_eye_rot_pushes, g_eye_rot_changes;
static float    g_eye_rot_worst_deg;

void kl_ovrp_set_eye_rotation(int eye, float qx, float qy, float qz, float qw) {
    if ((unsigned)eye > 1) return;
    // A zero quaternion is not a rotation; treat it as "none" rather than
    // collapsing the eye's basis to nothing.
    if (!(qx * qx + qy * qy + qz * qz + qw * qw > 1e-6f)) return;
    g_eye_rot_pushes++;
    {
        const float n[4] = { qx, qy, qz, qw };
        float d = klovrp_quat4_degrees(g_eye_rot[eye], n);
        // Not the first pair of pushes, which are identity -> the real cant and
        // always "a change" — the same exclusion the translation counter makes.
        if (g_eye_rot_pushes > 2 && d > 0.0f) {
            g_eye_rot_changes++;
            if (d > g_eye_rot_worst_deg) g_eye_rot_worst_deg = d;
        }
    }
    g_eye_rot[eye][0] = qx; g_eye_rot[eye][1] = qy;
    g_eye_rot[eye][2] = qz; g_eye_rot[eye][3] = qw;
}

// --- the eye transform, pinned for the guest's frame ------------------------
//
// The offsets and the cant together ARE device_from_view, and the frontend
// pushes both once per compositor iteration — i.e. at panel rate, faster than
// the guest's frame. A guest that reads the eye transform between its left and
// its right eye render therefore gets two different transforms, which is a
// per-eye disagreement in exactly the way the head pose was. So the pair is
// promoted at the guest's frame boundary alongside the frozen head pose (see
// klovrp_one_pose_open) and every query inside the frame answers from the
// promotion. Its own seqlock: the writer is the guest's frame driver, and half
// of one iteration's offsets with the other half's cant is a transform that
// never existed.
static struct { float off[2][3]; float rot[2][4]; } g_eye_frame;
static uint32_t g_eye_frame_seq;
static int      g_eye_frame_valid;

static void klovrp_eye_frame_promote(void) {
    uint32_t s = __atomic_load_n(&g_eye_frame_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_eye_frame_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(g_eye_frame.off, g_eye_off, sizeof g_eye_frame.off);
    memcpy(g_eye_frame.rot, g_eye_rot, sizeof g_eye_frame.rot);
    __atomic_store_n(&g_eye_frame_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_eye_frame_valid, 1, __ATOMIC_RELEASE);
}

// Fills `out` with the frame's eye transform and returns 1; 0 before the first
// promotion (boot), where the caller falls through to the live values.
static int klovrp_eye_frame_read(int eye, float off[3], float rot[4]) {
    if (!__atomic_load_n(&g_eye_frame_valid, __ATOMIC_ACQUIRE)) return 0;
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(&g_eye_frame_seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        float o[3], r[4];
        memcpy(o, g_eye_frame.off[eye], sizeof o);
        memcpy(r, g_eye_frame.rot[eye], sizeof r);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_eye_frame_seq, __ATOMIC_RELAXED) != s) continue;
        if (off) memcpy(off, o, sizeof o);
        if (rot) memcpy(rot, r, sizeof r);
        return 1;
    }
    return 0;
}

// The eye's cant as the guest sees it for this frame — the promoted one where
// there is one. Returns a pointer into caller-owned storage.
static const float *klovrp_eye_rot(int eye, float scratch[4]) {
    if ((unsigned)eye > 1) eye = 0;
    if (klovrp_eye_frame_read(eye, NULL, scratch)) return scratch;
    return g_eye_rot[eye];
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
    // The frame's promoted offsets where there are any, so both eyes and every
    // query inside one guest frame see ONE device_from_view. Falls through to
    // the live push before the first frame boundary.
    if ((unsigned)eye <= 1) {
        float o[3];
        if (klovrp_eye_frame_read(eye, o, NULL)) {
            *ox = o[0]; *oy = o[1]; *oz = o[2];
            return;
        }
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

// ...and the un-suffixed form, the pair shape once more:
// `float ovrp_GetUserIPD(void)`, the value in s0 and no arguments at all.
//
// The real 0x1309c0 is a WRAPPER around the 2-form — it stack-allocates a float,
// pre-zeroes it, calls ovrp_GetUserIPD2, and `fcsel`s between 0.0f and the
// written slot on `w0 < 0`. So it is not an independent measurement and must not
// be implemented as one; answering it from anywhere but klovrp_GetUserIPD2 is a
// stereo separation that can disagree with the one the eyes are rendered with.
//
// BONELAB calls it from managed code (the C# `OVRP_1_3_0.ovrp_GetUserIPD`
// binding), where the return convention is the whole contract — return
// OVRP_SUCCESS's 0 here by mistake and Unity gets an IPD of 0.0 rather than an
// error. Zero reads as "the eyes coincide", not as "nobody answered".
static float klovrp_GetUserIPD(void) {
    ovrp_hit("ovrp_GetUserIPD");
    float ipd = 0.0f;
    if ((int64_t)klovrp_GetUserIPD2(&ipd) < 0) return 0.0f;
    return ipd;
}

// ovrp_GetUserEyeHeight2(float *eyeHeight) — the standing height of the user's
// eye above the floor, in metres.
//
// **The ABI here is not guessed and not carried over from another title: it was
// read out of the plugin Pinball FX VR ships**, its libOVRPlugin.so at
// 0x177d60, and it is the IPD pair's shape instruction for
// instruction:
//
//     cbz  x0, +0x40           -> mov w0, #-1001   (NULL out-param)
//     ldr  x0, [x8, #0x7a0]
//     cbz  x0, +0x48           -> mov w0, #-1002   (no plugin)
//     ... blr; mov w0, wzr; str s0, [x19]          (writes *out, returns 0)
//
// So: ovrpResult in w0, the value through the pointer, -1001 on NULL, 0 on
// success. Not a float return, which is the un-suffixed spelling below.
//
// Pinball FX VR asks for this the instant it switches to a FloorLevel origin
// for its lobby — that is the exact pair of lines its device log ended on
// before this existed — because the number is how a Meta-fork UE builds the camera's floor
// offset. Until now it was an unimplemented name and the process died on it.
//
// Answered from klovrp_eye_height_configured, i.e. from KL_OVRP_EYE_HEIGHT, which
// is the SAME number klovrp_floor_offset raises the node positions by. That
// identity is the point: a guest that adds our answer to a pose we already
// offset, or subtracts it, has to land back where it started. Two independent
// standing heights is how the camera ends up 1.6 m out in whichever direction
// the guest happened to apply it.
// Defined with the tracking origin, several hundred lines below, because that
// is where the space it describes is argued about.
static float klovrp_eye_height_configured(void);
static float klovrp_eye_height(void);

// **KL_OVRP_USER_EYE_HEIGHT=standing|origin — which of two answers this
// gives.** Two titles reached this name independently and each was confirmed
// on the headset with its own answer:
//
//   origin   (DEFAULT) — klovrp_eye_height(): 0 under EyeLevel, the configured
//            height under FloorLevel/Stage — the offset the tracking-origin
//            family already applies, so the two cannot disagree about where
//            the floor is. Five Nights at Freddy's: Help Wanted 2.
//   standing — klovrp_eye_height_configured(): the standing height whatever
//            the origin, as argued above. Pinball FX VR's lobby wants this
//            one.
static int klovrp_user_eye_height_standing(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("KL_OVRP_USER_EYE_HEIGHT");
        v = (e && !strcmp(e, "standing")) ? 1 : 0;
    }
    return v;
}

static uint64_t klovrp_GetUserEyeHeight2(float *out) {
    ovrp_hit("ovrp_GetUserEyeHeight2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = klovrp_user_eye_height_standing() ? klovrp_eye_height_configured()
                                             : klovrp_eye_height();
    return OVRP_SUCCESS;
}

// ...and the un-suffixed form, `float ovrp_GetUserEyeHeight(void)`. The real
// 0x17ef20 is a WRAPPER around the 2-form and nothing else — stack slot zeroed,
// call, `fcsel s0, 0.0, slot, lt` — so it must be implemented as one here too.
// A second independent answer is a second standing height, which is the bug
// the note above exists to prevent.
static float klovrp_GetUserEyeHeight(void) {
    ovrp_hit("ovrp_GetUserEyeHeight");
    float h = 0.0f;
    if ((int64_t)klovrp_GetUserEyeHeight2(&h) < 0) return 0.0f;
    return h;
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

// The v1 shape of the same query, and a different ABI: ovrpFovf BY VALUE, a
// 4-float HFA home in s0..s3 (real 1.59 at 0x2885c — the uninitialised arm
// zeroes s0..s3, which is only ever a by-value struct). Same tangents, same
// metadata-pinned field order as the ...2 above; returning the struct from C
// makes Clang emit the real plugin's s-register return.
typedef struct { float up, down, left, right; } klovrp_fovf;
static klovrp_fovf klovrp_GetNodeFrustum(int node) {
    ovrp_hit("ovrp_GetNodeFrustum");
    const float *t = g_eye_tan[node == 1 ? 1 : 0];
    return (klovrp_fovf){t[2], t[3], t[0], t[1]};
}

// ovrpTrackingOrigin: EyeLevel=0, FloorLevel=1, Stage=2. This decides what
// space every pose we report is *in*, so it cannot stay a discarded argument
// on the generic yes-stub: under FloorLevel/Stage the guest expects the head
// to sit at standing eye height above y=0, and an identity (y=0) head puts the
// camera on the floor — the menu then renders above the view and the hands are
// below the floor, out of frustum. Recorded here and read by the pose defaults.
static int g_tracking_origin;          // 0 = eye level, until the guest says otherwise
int kl_ovrp_tracking_origin(void) { return g_tracking_origin; }

// ---- and the poses we file have to be IN the space we just named ----
//
// EyeLevel is not a preference, it is a coordinate system: y = 0 is at the
// user's eyes, so the head sits at y ~ 0. FloorLevel/Stage put y = 0 on the
// floor and the head a standing height above it. `klovrp_eye_height()` below is
// this runtime's own statement of that rule — it answers 0 under EyeLevel and
// KL_OVRP_EYE_HEIGHT under the others.
//
// The device pose stream broke the rule. `kl_ovrp_set_head_pose` is fed
// `queryDeviceAnchor`'s `originFromAnchorTransform` verbatim and ARKit's world
// origin sits near the FLOOR — so a guest that asked for EyeLevel was handed a
// head at y ~ 1.5, and everything it places AT its tracking origin landed a
// standing height below where it belongs. On Tetris Effect that is the symptom
// exactly: the menu, which is tracking-origin-relative, has to be looked down
// at, while the level, whose content is placed relative to the pawn rather than
// to the origin, is straight ahead. Its worn log settles that both scenes
// read one stream — `ovrp_SetTrackingOriginType2(arg=0)` twice from the same
// call site, `ovrp_RecenterTrackingOrigin2` never called at all.
//
// So when the guest has EXPLICITLY asked for EyeLevel, capture the head's
// height at that moment and subtract it from every pose we file, which is what
// "the origin is at your eyes" means. Gated on the setter having actually been
// CALLED: `g_tracking_origin` is 0 by default, and a port that never spoke to
// it must keep the poses it has always had. The offset is captured rather than
// assumed, so a seated player gets a seated origin.
//
//   KL_OVRP_ORIGIN_REBASE=0    off — the un-rebased poses, for the A/B
//   KL_OVRP_ORIGIN_Y=<metres>  a fixed offset instead of the captured one
//
// If this is backwards — if the menu was the scene placed right and the level
// is what moves — the level now sits a standing height too HIGH by the same
// amount, and KL_OVRP_ORIGIN_REBASE=0 is the entire revert.
static int   g_origin_set;        // the guest CALLED a SetTrackingOriginType
static int   g_origin_y_valid;
static float g_origin_y;          // metres subtracted from every published y

static int klovrp_rebase_wanted(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_ORIGIN_REBASE", 1);
    return on && g_origin_set && g_tracking_origin == 0;
}

// The offset in force, or 0. The publishers subtract it; kl_ovrp_get_head_pose
// adds it back, because the FRONTEND's question is still asked and answered in
// the frontend's own space and only the guest lives in the rebased one.
static float klovrp_origin_y(void) {
    if (!klovrp_rebase_wanted() || !g_origin_y_valid) return 0.0f;
    return g_origin_y;
}

static void klovrp_origin_capture(float py) {
    if (!klovrp_rebase_wanted() || g_origin_y_valid) return;
    float forced = kl_env_float("KL_OVRP_ORIGIN_Y", -1e9f);
    g_origin_y = forced > -1e8f ? forced : py;
    g_origin_y_valid = 1;
    fprintf(stderr, "  [ovrp] tracking origin EyeLevel: y=0 is now the head's "
                    "height at this moment, %.3f m%s. Every pose filed for the "
                    "guest drops by that much, so its origin-relative content "
                    "sits at eye level instead of a standing height below it. "
                    "KL_OVRP_ORIGIN_REBASE=0 reverts.\n",
            g_origin_y, forced > -1e8f ? " (forced by KL_OVRP_ORIGIN_Y)" : "");
}

// A recenter, or a change of origin mode, re-takes the capture. Neither happens
// on this title — the guest polls GetAppShouldRecenter2 14,273 times and never
// acts on it — but a runtime that names a space has to be able to move it.
static void klovrp_origin_recenter(void) {
    if (!g_origin_y_valid) return;
    g_origin_y_valid = 0;
    fprintf(stderr, "  [ovrp] tracking origin recentred — the next pose sets y=0\n");
}

// The raw (frontend-space, floor-relative) head height of the last publish,
// BEFORE any rebase — what the origin trail and the quad placement lines quote.
static float g_raw_head_y;
static int   g_raw_head_have;

static const char *klovrp_origin_name(int o) {
    return o < 0 ? "(never set)" : o == 0 ? "EyeLevel" : o == 1 ? "FloorLevel"
         : o == 2 ? "Stage" : "?";
}

// **The origin trail.** One line per CHANGE of the guest's tracking origin,
// wall-clock stamped so it lines up with the boot log's [cp] lines and (UTC
// offset aside) with Thumper.log's LoadMap/FSplash lines. ovrp_log_arg names
// only the FIRST call per value, and FNAF HW2 flips EyeLevel <-> FloorLevel
// ~37 times a session (Floor at every LoadMap, Eye ~0.5 s later, Floor again
// ~3 s after that, Eye for every loading screen) — which phase a misplaced
// screen fell in is unanswerable without this. KL_OVRP_ORIGIN_TRAIL=0 silences.
static void klovrp_origin_trail(int from, int to) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_ORIGIN_TRAIL", 1);
    if (!on || from == to) return;
    static unsigned n;
    if (++n > 400) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t t = ts.tv_sec;
    localtime_r(&t, &tm);
    fprintf(stderr, "  [ovrp] origin #%u %02d:%02d:%02d.%03ld %s -> %s (raw head "
                    "%.3f m above the floor%s; rebase in force until now %.3f m)%s\n",
            n, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
            klovrp_origin_name(from), klovrp_origin_name(to),
            g_raw_head_have ? g_raw_head_y : 0.0f,
            g_raw_head_have ? "" : " — no head published yet",
            klovrp_origin_y(),
            n == 400 ? " — trail capped at 400 lines" : "");
}
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
// the call failed. Giving it an `int *` and storing through it is a wild store
// from a getter — whatever the guest happened to leave in x0 gets written, which
// is a SIGBUS or silent corruption depending on the register, unattributable to
// this function from anywhere.
static uint64_t klovrp_SetTrackingOriginType(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType");
    ovrp_log_arg("ovrp_SetTrackingOriginType", origin, __builtin_return_address(0));
    klovrp_origin_trail(g_origin_set ? g_tracking_origin : -1, origin);
    if (g_tracking_origin != origin) klovrp_origin_recenter();
    g_tracking_origin = origin;
    g_origin_set = 1;
    return OVRP_TRUE;                       // ovrpBool
}
static uint64_t klovrp_SetTrackingOriginType2(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType2");
    ovrp_log_arg("ovrp_SetTrackingOriginType2", origin, __builtin_return_address(0));
    klovrp_origin_trail(g_origin_set ? g_tracking_origin : -1, origin);
    if (g_tracking_origin != origin) klovrp_origin_recenter();
    g_tracking_origin = origin;
    g_origin_set = 1;
    return OVRP_SUCCESS;                    // ovrpResult
}
// The plugin's own event queue, which OVRManager drains every frame.
//
// Signature read straight out of the real 1.59 body (+0x266dc), because both
// arguments are pointers it WRITES:
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
    // ...and whether those six mean anything. Zero is not "unknown", it is the
    // assertion that the thing is STATIONARY — a claim no frontend that
    // publishes a pose without motion is entitled to make about a controller
    // whose position is visibly changing. OpenXR carries the same distinction
    // in XrSpaceVelocity.velocityFlags, which kl_openxr answers from this.
    int   motion_valid;
    // Which frontend pose PUSH this sample came from — an opaque counter the
    // frontend stamps through kl_ovrp_set_head_pose_tag just before it writes
    // the pose (see kl_ovrp.h). It rides in the struct for the same reason the
    // velocity does: every latch in this file copies a klovrp_pose by value, so
    // a tag that lives here stays welded to the orientation it names through
    // the seqlock, the per-frame freeze and the per-step ring alike. 0 means
    // "no frontend stamped this", which is every host run.
    uint64_t tag;
} klovrp_pose;
static klovrp_pose g_head_pose = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0,   // motion_valid: nothing has been published, so there is nothing to know
    0,   // tag: no frontend has stamped a push
};
static int g_head_set;              // has a frontend ever written a head pose?
// The two hands, published by the same frontend in the same breath. Declared
// here beside the head because the per-frame latch below promotes all three
// together — they are one sample of one instant and must stay so.
// CLOCK_MONOTONIC nanoseconds — the clock the frame record is stamped on, and
// the same one the guest's own System.nanoTime answers from, so an age computed
// against it is not the offset between two clocks.
static uint64_t klovrp_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static klovrp_pose g_hand_pose[2];

// --- The frontend/guest pose handoff: a seqlock ----------------------------
//
// A torn read is an INCOHERENT pose — half of one frame's rotation with half of
// another's, a pose that never existed — not merely a stale one. kl_reproject.c
// subtracts the pose a frame was rendered with from the pose it is displayed
// at, so a torn latch is a wrong delta and a wrong delta is a visible jump.
// Writer and reader are different threads on both frontends.
//
// A seqlock rather than a mutex, because the readers are on the guest's hot
// path — every ovrp_GetNodePoseState — while the writer is one thread at
// display rate. The retry count is bounded: past it the read is taken
// unsynchronised, so a descheduled writer costs one incoherent pose rather than
// a spin inside a frame.
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

// `when` is the instant the SAMPLE IS ABOUT, not the instant this call happens
// to be made at. 0 means "this publisher has no such clock", and then the call
// time is the only answer available. See klovrp_derive_motion for why the
// distinction is the difference between a velocity and a scheduling artefact.
static void klovrp_hist_note(klovrp_motion_hist *h, const klovrp_pose *v,
                             double when) {
    h->have = 1;   h->t  = when > 0 ? when : klovrp_mono_now();
    h->px = v->px; h->py = v->py; h->pz = v->pz;
    h->qx = v->qx; h->qy = v->qy; h->qz = v->qz; h->qw = v->qw;
}

// Fill v's velocity from the difference against `h`, then advance `h`.
//
// **THE dt HAS TO BE THE SAMPLES' OWN dt**, and this was a real bug rather
// than a tidy-up. The numerator is the difference between two poses the
// compositor obtained by asking ARKit for the device anchor AT TWO PRESENTATION
// TIMES (KleptonCompositor.updatePoses(at:)) — a uniform grid, one panel slot
// apart. The denominator was CLOCK_MONOTONIC differenced across the two *calls*,
// i.e. the wall-clock spacing of the frame loop's publish step. Those are not
// the same interval: the first is uniform by construction, the second carries
// every bit of scheduling jitter the render thread suffers. Dividing a uniform
// numerator by a jittery denominator multiplies the velocity by
// (dt_presentation / dt_call) — a per-frame noise term riding on a pose
// series that is itself clean. A
// guest that integrates that velocity to predict its render pose then gets a
// different answer every frame for the same real motion.
//
// So a publisher that KNOWS what instant its sample is about says so, and the
// difference is taken on that clock. Publishers with no such clock — the macOS
// viewer, the parked default poses — pass 0 and keep the call-time behaviour,
// which for them is the only clock there is.
static void klovrp_derive_motion(klovrp_pose *v, klovrp_motion_hist *h,
                                 double when) {
    static int derive = -1;
    if (derive < 0) derive = kl_env_on("KL_OVRP_DERIVE_VELOCITY", 1);

    double now = when > 0 ? when : klovrp_mono_now();
    double dt  = h->have ? now - h->t : 0.0;

    v->vx = v->vy = v->vz = v->avx = v->avy = v->avz = 0;
    v->motion_valid = 0;

    // A plausible frame's worth of time, and outside it the difference is not a
    // velocity: below the floor it is float noise divided by nearly zero, and
    // above the ceiling the two samples are not consecutive motion at all (a
    // paused guest, a first sample, a frontend that stopped publishing). Both
    // cases report UNKNOWN rather than a number.
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
    klovrp_hist_note(h, v, now);
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
// The configured standing eye height itself, ORIGIN-INDEPENDENT: how far the
// eyes are above the floor. klovrp_eye_height() below is the *offset to apply
// to a default head*, which is that height under FloorLevel/Stage and zero
// under EyeLevel — two different questions that were one function until the
// tracking-transform family (klovrp_origin_height) needed to ask the first one
// while the guest was running the second.
static float klovrp_eye_height_configured(void) {
    static int init;
    static float h = 1.6f;
    if (!init) {
        init = 1;
        h = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    }
    return h;
}

static float klovrp_eye_height(void) {
    return g_tracking_origin == 0 ? 0.0f : klovrp_eye_height_configured();
}

float kl_ovrp_eye_height(void) { return klovrp_eye_height(); }

// --- The floor offset, and why the space we serve is NOT the space we think --
//
// Everything above is written as though the tracking space were floor-relative
// — kl_ovrp.h says so, kl_openxr.c's STAGE/LOCAL model assumes it, and
// klovrp_eye_height raises the DEFAULT head by the standing height to make it
// true. It is true only while there is no real tracker. **The moment ARKit
// starts publishing, it stops being true**, and nothing noticed because
// nothing asked.
//
// visionOS's WorldTrackingProvider puts its world origin at the device's pose
// when the session starts, gravity-aligned. A worn Vision Pro therefore reports
// a head at y ~= 0 — the origin is at the EYE, not on the floor — and
// kl_ovrp_set_head_pose stores that number raw. klovrp_eye_height's raise is
// guarded on `!g_head_set`, so it applies to exactly the head that ARKit has
// replaced and to no other.
//
// Under EyeLevel that is right by construction: the guest expects the eye at
// the origin and the eye is at the origin. Under FloorLevel the guest expects
// the head to stand ~1.6 m above y=0 and we hand it ~0. **The camera is in the
// floor**, which is what Pinball FX VR showed the moment it reached its lobby
// and switched origins:
//
//     [ovrp] ovrp_SetTrackingOriginType2(arg=0/0x0)   <- splashes, loading
//     ...
//     [ovrp] ovrp_SetTrackingOriginType2(arg=1/0x1)   <- the lobby
//     [klepton] fatal: ... 'ovrp_GetUserEyeHeight2'
//
// — and the wearer looked at the floor before the lobby's stairs from inside
// it. Both are the same bug seen twice: the
// title switched to a floor origin and then asked us how high the user's eyes
// are, because that is the number it needs to place the camera, and we had
// neither the offset nor the answer.
//
// **Applied at the guest's read, not at the frontend's write, and that is the
// whole care in this change.** The recorded render pose (klovrp_BeginFrame) is
// reprojected by KleptonCompositor against a RAW ARKit `originFromDevice`
// transform that never passes through this file. Offsetting the stored pose
// would put 1.6 m of translation into every reprojection — a correct-looking
// picture in the wrong place, on every port. So the internal space stays
// exactly what ARKit gave us, and the offset is added once, at the single line
// where a node position is written into the guest's own struct.
//
// **Off by default, and selected by nothing.** Shipped once alongside
// ovrp_GetUserEyeHeight2, the pair measured on the headset moved the camera
// from IN the lobby floor to floating well ABOVE it. The two halves
// double-add, and the reason is the getter's whole purpose. A guest asks how
// tall the user is precisely so it can build its own floor-relative offset out
// of that number; once it has an answer it adds ~1.6 m itself, and a runtime
// that has already pre-raised the poses hands it ~3.2 m of eye height. The
// getter is the fix; this is the overshoot.
//
// Kept in the tree, and kept gated, because the opposite guest exists: one that
// reads a FloorLevel pose and trusts it without ever asking our height. For
// that guest this knob is the fix and the getter is irrelevant. Nothing selects
// it today; build/t_ovrpfloor asserts BOTH directions so neither can rot.
static int klovrp_floor_offset_enabled(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_FLOOR_OFFSET", 0);
    return on;
}

// The metres to add to a node position before the guest sees it. Zero unless
// the knob is on, the guest asked for a non-eye-level origin, AND a real
// tracker has replaced the default head — that last condition is what stops
// this double-counting klovrp_eye_height's raise, which applies to precisely
// the head this one does not.
static float klovrp_floor_offset(void) {
    if (!klovrp_floor_offset_enabled()) return 0.0f;
    if (g_tracking_origin == 0) return 0.0f;
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) return 0.0f;
    return klovrp_eye_height_configured();
}

// --- One pose per guest frame, and why the live one is wrong ---------------
//
// **This is what was left of the judder after the swapchain was fixed.**
//
// The frontend publishes a new pose every *display* frame, on its own thread
//. The guest reads poses through ovrp_GetNodePoseState
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
        // **Default ON.** It was written that way (`on = !(e && e[0] == '0')`)
        // and became `kl_env_on(..., 0)` in a tidying pass, which inverted the
        // knob's meaning against both its own comment above and its entry in
        // DEBUG_ENV_VARS — each of which describes `=0` as the OVERRIDE. With
        // the latch off the guest reads a live pose that moves DURING its own
        // frame, so the pose recorded for timewarp is not the pose the picture
        // was drawn from, and the composite corrects by the difference. That is
        // the temporal doubling this was written to fix, and it scales with the
        // frame time — invisible at 90 fps, a full second of head motion during
        // a 1 fps shader prewarm.
        on = kl_env_on("KL_OVRP_LATCH", 1);
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
    // Every guest-facing head read reaches this function (the one-pose freeze,
    // the frame latch, Update2's fallback, klovrp_head).
    return h;
}

// One pose per guest frame — the layer above this one. Defined below, because
// it reports its divergence against klovrp_head_published() and measures it
// with klovrp_quat_degrees; declared here because klovrp_head() is the first
// thing that has to consult it. `who` is the entry point, for the counters.
enum { KLOP_UPDATE2, KLOP_STEP, KLOP_HEAD, KLOP_EYEVIEW, KLOP_N };
static int klovrp_one_pose_get(klovrp_pose *out, int who);

// The head pose as the GUEST sees it: pinned for the whole of its frame.
static klovrp_pose klovrp_head(void) {
    // The frame's frozen pose first, when one-pose is running: it is the same
    // answer this function's own latch would give in the good case and the
    // RIGHT answer in the case the latch never covered — a query arriving on a
    // path (a step sample, an eye view) that reaches around klovrp_head().
    {
        klovrp_pose op;
        if (klovrp_one_pose_get(&op, KLOP_HEAD)) return op;
    }
    if (klovrp_latch_enabled() && __atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        klovrp_pose h = klovrp_seq_read(&g_frame_head, &g_frame_pose_seq);
        if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
        return h;
    }
    return klovrp_head_published();
}

// --- The tracking-transform family: where one origin sits inside another ----
//
// ABI read out of Batman: Arkham Shadow's own 1.60 libOVRPlugin.so, not
// assumed:
//
//   1ccbb0  ovrp_GetCurrentTrackingTransformPose (ovrpPosef *x0)
//   1ccbe0  ovrp_GetTrackingTransformRawPose     (ovrpPosef *x0)
//   1ccc10  ovrp_GetTrackingTransformRelativePose(ovrpPosef *x0, ovrpTrackingOrigin w1)
//   1c9730  ovrp_GetTrackingCalibratedOrigin2    (ovrpPosef *x0)
//
// Every one of them opens `cbz x0 -> mov w0, #-1001` and relays through the
// backend vtable (+0x4e8 / +0x4f0 / +0x4f8 / +0x158). **The pose is an
// out-pointer, not an HFA return** — no `movi v0` / `stp q` return path here,
// unlike ovrp_GetNodeFrustum next door. The relative form is the one worth
// reading twice: its shuffle is `mov x8, x0; mov w2, w1; mov x1, x8`, i.e. the
// OUT-POINTER COMES FIRST and the origin enum second, in w1. Transposing that
// pair would store seven floats through an enum value. GetTrackingCalibratedOrigin2
// settles the struct size too: `str q0, [x19]` + `stur q1, [x19, #0xc]` = 28
// bytes = seven floats, ovrpQuatf{x,y,z,w} then ovrpVector3f{x,y,z},
// ORIENTATION FIRST.
//
// --- and what the transform actually is here --------------------------------
//
// The relative form answers "where does the origin you NAME sit, expressed in
// the origin I am CURRENTLY running?". This runtime has exactly one tracking
// space — ARKit's, whose y=0 is at the floor (KleptonCompositor hands
// queryDeviceAnchor's originFromAnchorTransform to kl_ovrp_set_head_pose
// verbatim) — so the whole family is derived from the one rule this file
// already states in klovrp_eye_height: FloorLevel(1) and Stage(2) put y=0 on
// the floor, EyeLevel(0) puts it at the user's eyes.
//
//   height(FloorLevel) = height(Stage) = 0
//   height(EyeLevel)   = the eyes' height above the floor
//   relative(o)        = height(o) - height(current)
//
// Identity when the guest names the origin it is already in — which is this
// case for Batman: Arkham Shadow, which calls
// ovrp_SetTrackingOriginType2(FloorLevel) once at boot, and our poses ARE
// floor-space, so the two agree and
// nothing is being invented. A different origin gets the floor/eye offset, and
// only the y component: nothing here rotates or translates the space sideways.
//
// The eye height is CAPTURED from the live head the first time it is asked for
// rather than assumed, so a seated player gets a seated origin — the same
// choice the Tetris Effect origin rebase made. It is captured once and never
// re-read, because a transform between two origins that wobbled with the
// player's head bob would be wrong in a way no guest could correct for.
//
//   KL_OVRP_ORIGIN_RELATIVE=0   answer identity for every origin, which is the
//                               whole revert if the offset turns out to be
//                               applied on top of poses that already carry it.
//   KL_OVRP_EYE_HEIGHT=<m>      the fallback used before any head is published.
static float klovrp_eye_origin_height(void) {
    static int   have;
    static float h;
    if (!have) {
        have = 1;
        h = klovrp_eye_height_configured();
        if (__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) {
            float y = klovrp_head_published().py;
            // A published head below the ankles or above a doorway is not an
            // eye height; keep the configured one rather than build a
            // coordinate system on a bad sample.
            if (y > 0.2f && y < 3.0f) h = y;
        }
    }
    return h;
}

// How far the named origin's y=0 sits above the floor, in the one space we have.
static float klovrp_origin_height(int origin) {
    return origin == 0 ? klovrp_eye_origin_height() : 0.0f;
}

static int klovrp_origin_relative_enabled(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_ORIGIN_RELATIVE", 1);
    return on;
}

// ovrpPosef out: identity orientation, and a pure vertical offset.
static uint64_t klovrp_pose_out_y(float *out, float y) {
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;   // quat x,y,z,w
    out[4] = 0.0f; out[5] = y;    out[6] = 0.0f;                  // position
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetTrackingTransformRelativePose(float *out, int origin) {
    ovrp_hit("ovrp_GetTrackingTransformRelativePose");
    ovrp_log_arg("ovrp_GetTrackingTransformRelativePose", origin,
                 __builtin_return_address(0));
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    float y = 0.0f;
    if (klovrp_origin_relative_enabled())
        y = klovrp_origin_height(origin) - klovrp_origin_height(g_tracking_origin);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ovrp] GetTrackingTransformRelativePose(origin=%d) while "
                        "running origin %d -> y=%+.3f m%s. Our one tracking space is "
                        "ARKit's, whose y=0 is the floor, so FloorLevel/Stage are it "
                        "and EyeLevel sits an eye height above. KL_OVRP_ORIGIN_RELATIVE=0 "
                        "forces identity.\n",
                origin, g_tracking_origin, y,
                klovrp_origin_relative_enabled() ? "" : " (forced identity)");
    }
    return klovrp_pose_out_y(out, y);
}

// The current transform, and the raw (pre-recentre) one. Identity both: there is
// no second, un-recentred space behind ours to describe an offset to — nothing
// in this runtime ever recentres (ovrp_RecenterTrackingOrigin is a recorded
// no-op), so raw and current are the same space, and the space is the one every
// pose we file is already in.
static uint64_t klovrp_GetCurrentTrackingTransformPose(float *out) {
    ovrp_hit("ovrp_GetCurrentTrackingTransformPose");
    return klovrp_pose_out_y(out, 0.0f);
}
static uint64_t klovrp_GetTrackingTransformRawPose(float *out) {
    ovrp_hit("ovrp_GetTrackingTransformRawPose");
    return klovrp_pose_out_y(out, 0.0f);
}
static uint64_t klovrp_GetTrackingCalibratedOrigin2(float *out) {
    ovrp_hit("ovrp_GetTrackingCalibratedOrigin2");
    return klovrp_pose_out_y(out, 0.0f);
}

// The shortest angle between two orientations, in degrees. Used only to report
// how much motion the latch is absorbing.
static float klovrp_quat_degrees(const klovrp_pose *a, const klovrp_pose *b) {
    // qx..qw are contiguous in klovrp_pose, so this is the same four floats the
    // eye-cant counter compares — one implementation, not two.
    return klovrp_quat4_degrees(&a->qx, &b->qx);
}

// --- How far apart the poses handed out INSIDE one guest frame can be -------
//
// The question the latch exists to answer, measured rather than assumed: over
// one guest frame, what is the largest angle between any two head poses this
// runtime returned to the guest? If it is nonzero the guest's two eyes — or its
// camera and its world — can embody different head orientations, which is a
// doubling mechanism in its own right and one no reprojection can undo.
//
// Zero is the expected reading for an OVRPlugin guest even with the frame latch
// off, because every answer comes from the Update2 step sample. A nonzero
// reading means some path is reaching the live published pose.
static klovrp_pose g_ifs_ref;
static int         g_ifs_have;
static float       g_ifs_worst;      // this frame
static float       g_ifs_worst_run;  // worst over the reporting window

static void klovrp_ifs_observe(const klovrp_pose *h) {
    if (!__atomic_load_n(&g_ifs_have, __ATOMIC_ACQUIRE)) {
        g_ifs_ref = *h;
        __atomic_store_n(&g_ifs_have, 1, __ATOMIC_RELEASE);
        return;
    }
    float d = klovrp_quat_degrees(&g_ifs_ref, h);
    if (d > g_ifs_worst) g_ifs_worst = d;
}

static void klovrp_ifs_frame(void) {
    if (g_ifs_worst > g_ifs_worst_run) g_ifs_worst_run = g_ifs_worst;
    g_ifs_worst = 0;
    __atomic_store_n(&g_ifs_have, 0, __ATOMIC_RELEASE);
}

// --- ONE POSE PER GUEST FRAME (KL_OVRP_ONE_POSE) ----------------------------
//
// **What was left of the head-turn judder.** A uniform frame clock made no
// visual change, so dt quantization is not the fault; filing each frame's
// render pose under the guest's own frame index measured `filed-at-lag 0x
// 100%` — the BeginFrame race never fired — so that is not the fault either.
// But the same run printed `intra-frame pose spread: worst 2.973 deg`: inside
// ONE guest frame, the head poses this runtime handed the guest differed by up
// to three degrees.
//
// That is the whole mechanism. A real OVR runtime answers every pose query
// inside frame N from ONE predicted pose for frame N's display time. This
// runtime answers from live-ish sources — the frontend publishes at panel rate
// on its own thread, and the guest's queries are scattered across a 13.9 ms
// window: Update2(Physics) early, Update2(Render) later, twenty
// GetNodePoseState reads between them, the eye view later still. Each one gets
// a different answer. The pixels then embody some MIXTURE of those answers,
// while reprojection subtracts the single pose we filed — so the picture is
// displaced by whatever the difference happened to be, and the difference
// varies with scheduling from frame to frame. Alternating displacement of a
// degree or so, growing with head speed, fusing at rest: the artifact.
//
// It also explains the spread across titles by window width alone. SUPERHOT at
// 120 Hz has an 8.3 ms window to scatter in and is smooth; Pistol Whip at 72 Hz
// has 13.9 ms and shakes moderately; ETT at 72 Hz with the worst call pattern
// shakes worst.
//
// The fix is to stop having a "now" inside a guest frame at all. At the frame's
// boundary one head pose is frozen, and EVERY head-derived answer for the rest
// of that frame — both Update2 steps, every GetNodePoseState, the eye view, the
// OpenXR head query — returns it. The record filed for reprojection is then the
// same pose by construction rather than by luck, because it was the only pose
// in the frame.
//
// **What is frozen, and what is not.** Head and everything derived from it
// (the eyes) freeze; so does the eye transform itself (klovrp_eye_frame_promote
// — see there for why device_from_view has the same problem in miniature).
// Hands and controllers stay LIVE: they are not world-lock relevant, and
// pinning them would buy nothing but input latency.
//
// **Prediction, and what shipped.** The frozen pose is the LATCHED SAMPLE — the
// published pose as it stood at the frame boundary — not an extrapolation to
// kl_ovrp_predicted_display_time(). Constancy is the property being tested;
// extrapolation is a second variable and a second way to be wrong, and a wrong
// extrapolation looks like the very artifact under investigation. The material
// is there if it is ever wanted: klovrp_derive_motion already
// fills vx/vy/vz and avx/avy/avz on every published head pose, and those are
// the fields the guest is handed at +0x1c and +0x34, so the guest's own
// predictor is being fed real velocities today.
//
// Gate: KL_OVRP_ONE_POSE. Default ON — this is runtime-semantics correctness,
// not an experiment. `0` / `live` / `off` restores the per-query live answers
// for the A/B; `1` / `index` is the default. Documented in DEBUG_ENV_VARS.md.
static klovrp_pose g_op_pose;
static uint32_t    g_op_seq;
static int         g_op_valid;
static int         g_op_index;          // the guest frame index it was frozen for
// Set once the guest has opened a frame by NAME (an Update2 carrying a usable
// frame index). From then on that is the only boundary, and kl_ovrp_frame_latch
// stops opening frames of its own — two boundaries in one guest frame would
// freeze two poses in it, which is the bug wearing the fix's clothes.
static int         g_op_index_keyed;

// --- The boundary is the guest's frame CYCLE, not its frame INDEX -----------
//
// **What Paint the Town Red VR measured, and it is the whole artifact.** The
// one-pose census counts how many ovrp_Update2/3 calls each frozen pose served.
// This guest calls ovrp_Update3 exactly TWICE per frame (2614 calls over 1307
// frames — the Render step and the Physics step), so a pose that is frozen
// once per guest frame must serve exactly 2. Two runs both say:
//
//     menu, smooth          served update2 588..600 per 300 freezes  -> 1.00
//     gameplay, judders     served update2 350..369 per 300          -> 1.70
//
// In gameplay Unity's render thread TRAILS its sim thread, so the two Update3
// calls of one frame arrive carrying DIFFERENT frame indices — Physics with N,
// Render with N-1. Keying the freeze on the index therefore sees a change and
// RE-FREEZES in the middle of the frame: the ~26 ovrp_GetNodePoseState3 calls
// of that frame get answered from TWO different head poses, split at whichever
// instant the second Update3 landed, and that split point moves with thread
// scheduling every single frame. The pixels embody a mixture that changes frame
// to frame while the anchor we file is one of the two. Sub-degree, alternating,
// scaling with head speed, fusing at rest — the artifact, exactly, and the same
// one the block above was written to remove. `live-divergence worst` agrees:
// 0.056-0.46 deg in the menu, 0.71-1.12 deg in gameplay.
//
// The menu is smooth at the SAME eye size because there the two Update3 calls
// carry the same index and the invariant holds.
//
// **The fix.** Stop keying on a name the two threads do not agree on. The guest
// delimits its own frame cycle: ovrp_WaitToBeginFrame is called exactly once
// per frame (1307 for 1307 frames) and is already documented here as the
// frame's first instruction. It ARMS the boundary; the first
// klovrp_one_pose_open of the cycle spends the arm and freezes, and every later
// one in the same cycle reuses that pose whatever index it carries.
//
// A guest that never calls WaitToBeginFrame (the OpenXR path, a headless run)
// never arms, so it keeps the index keying it has always had —
// g_op_arm_driven is the discriminator.
//
// KL_OVRP_POSE_BOUNDARY=cycle (default) | index restores the index keying, for
// an A/B without a rebuild.
static int         g_op_armed;          // a frame cycle has opened and not yet frozen
static int         g_op_arm_driven;     // this guest delimits its own cycles
static uint64_t    g_op_freezes;        // opens that took a new pose
static uint64_t    g_op_reuses;         // opens that correctly reused the frame's
static uint64_t    g_op_arms;           // frame cycles seen

static int klovrp_pose_boundary_cycle(void) {
    static int on = -1;
    if (on < 0) {
        const char *m = kl_env_str("KL_OVRP_POSE_BOUNDARY", "cycle");
        on = !(m && (m[0] == 'i' || m[0] == 'I'));   // "index" turns it off
        fprintf(stderr, "  [ovrp] pose boundary = %s (KL_OVRP_POSE_BOUNDARY=%s)"
                        " — the frozen head pose is renewed %s. Read"
                        " `poses/frame` on the one-pose census: it MUST be"
                        " 1.00; index keying measured 1.70 in gameplay and 1.00"
                        " in the menu, which is the smooth/judder split.\n",
                on ? "guest frame CYCLE" : "guest frame INDEX", m ? m : "cycle",
                on ? "once per ovrp_WaitToBeginFrame"
                   : "whenever ovrp_Update2/3 carries a new frame index");
    }
    return on;
}

// Called from the guest's own once-per-frame entry point. See above.
static void klovrp_one_pose_arm(void) {
    // The intra-frame spread window is the same window, now that the guest is
    // telling us where its frames start: measured between cycles, `intra-frame
    // pose spread` should read 0.000 deg once the boundary is right, because
    // every query in the cycle is answered from one pose.
    klovrp_ifs_frame();
    __atomic_store_n(&g_op_arm_driven, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_op_armed, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_op_arms, 1, __ATOMIC_RELAXED);
}

static int klovrp_one_pose_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *m = kl_env_str("KL_OVRP_ONE_POSE", "index");
        // "live", "off" and "0" all mean answering every query live.
        on = !(m && (m[0] == 'l' || m[0] == 'L' || m[0] == '0' ||
                     m[0] == 'o' || m[0] == 'O'));
        fprintf(stderr, "  [ovrp] one pose per guest frame %s (KL_OVRP_ONE_POSE"
                        "=%s) — every head query inside a guest frame %s. Head "
                        "velocity IS populated (linear +0x1c, angular +0x34, "
                        "derived in klovrp_derive_motion); acceleration is not.\n",
                on ? "ON" : "off", m ? m : "index",
                on ? "answers from one pose frozen at the frame boundary"
                   : "answers from whatever was live at the moment it asked");
    }
    return on;
}

// Per entry point: how many answers this frame's frozen pose served, and how
// far it had already drifted from the live published pose by the time each one
// was answered. That divergence is exactly what the pixels WOULD have embodied
// differently without this — so if it stays around 3 deg while the shaking is
// gone, the mechanism is confirmed with a number rather than an argument.
static uint64_t g_op_served[KLOP_N];
static uint64_t g_op_fellthrough;       // answered live: before the first freeze
static float    g_op_div_worst;
static double   g_op_div_sum;
static uint64_t g_op_div_n;

// --- The frozen SERIES ------------------------------------------------------
//
// The counters above measure one frozen pose against something else. This one
// measures the frozen poses AGAINST EACH OTHER: the compositor obtains each of them by asking ARKit for the device
// anchor at a per-slot presentation time, so if consecutive frames end up
// keyed to time bases that alternate, the series we freeze walks
// forward-back-forward through time while every individual pose is perfectly
// valid. A guest rendering that series draws a world that zigzags, and no
// warp, no ordering fix and no projection audit can see it.
//
// So: the angle between consecutive frozen head poses, and the SHAPE of that
// step sequence.
//
//   mean step        — mean |delta| over the window, degrees. At a steady yaw
//                      rate w and 90 Hz this should be about w * 11.1 ms.
//   alternation      — mean(|d_i - d_{i-1}|) / mean(d). About 0 for a series
//                      stepping uniformly; 1 or more for an A-B-A-B pattern
//                      (the signature of two interleaved time bases).
//   sign flips       — the percentage of consecutive steps whose rotation
//                      REVERSED about the dominant axis while the head was
//                      actually moving. A head being turned smoothly cannot
//                      reverse 40 times a second; the series can.
//   worst reversal   — the largest single step that was a reversal, degrees.
//
// The gate: a step pair is only counted toward the flip percentage when BOTH
// its steps exceed 0.05 deg. At rest the series is float noise about a fixed
// pose and its sign flips at random, which would drown the reading in exactly
// the condition the artefact does not occur in.
//
// Reading it: a smooth series (alternation near 0, flips near 0) with the shake
// still present rules the series out and leaves the synthesized velocities
// (KL_OVRP_ZERO_HEAD_VEL below). A zigzag series convicts it with a number,
// whatever the picture does.
static klovrp_pose g_ser_prev;          // the previous frame's frozen pose
static int         g_ser_have;
static float       g_ser_prev_step;     // |delta| of the previous step, deg
static float       g_ser_prev_dom;      // its dominant-axis component, signed
static int         g_ser_prev_axis;     // which axis that was
static double      g_ser_step_sum;      // for mean step
static uint64_t    g_ser_step_n;
static double      g_ser_alt_sum;       // for the alternation index
static uint64_t    g_ser_alt_n;
static uint64_t    g_ser_flip_n, g_ser_flip_denom;
static float       g_ser_worst_rev;

// The rotation FROM a TO b as a signed axis-angle vector in degrees — the same
// arithmetic klovrp_derive_motion does, kept separate because that one divides
// by a dt and this one must not (a series step is an angle, not a rate).
static void klovrp_step_vec(const klovrp_pose *a, const klovrp_pose *b,
                            float out[3]) {
    float cx = -a->qx, cy = -a->qy, cz = -a->qz, cw = a->qw;
    float dw = b->qw*cw - b->qx*cx - b->qy*cy - b->qz*cz;
    float dx = b->qw*cx + b->qx*cw + b->qy*cz - b->qz*cy;
    float dy = b->qw*cy - b->qx*cz + b->qy*cw + b->qz*cx;
    float dz = b->qw*cz + b->qx*cy - b->qy*cx + b->qz*cw;
    if (dw < 0) { dw = -dw; dx = -dx; dy = -dy; dz = -dz; }   // short way round
    float s = sqrtf(dx*dx + dy*dy + dz*dz);
    if (s > 1e-9f) {
        float k = 2.0f * atan2f(s, dw) * (180.0f / 3.14159265358979f) / s;
        out[0] = dx * k; out[1] = dy * k; out[2] = dz * k;
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}

// Called with each newly frozen pose, in freeze order, from the one thread that
// freezes them (the guest's frame boundary).
static void klovrp_series_note(const klovrp_pose *h) {
    if (!g_ser_have) { g_ser_prev = *h; g_ser_have = 1; return; }

    float v[3];
    klovrp_step_vec(&g_ser_prev, h, v);
    g_ser_prev = *h;

    float mag = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    int   ax  = 0;
    if (fabsf(v[1]) > fabsf(v[ax])) ax = 1;
    if (fabsf(v[2]) > fabsf(v[ax])) ax = 2;

    g_ser_step_sum += (double)mag;
    g_ser_step_n++;

    if (g_ser_step_n > 1) {
        g_ser_alt_sum += (double)fabsf(mag - g_ser_prev_step);
        g_ser_alt_n++;
        // The flip test compares the two steps about the PREVIOUS step's
        // dominant axis, so a step whose dominant axis differs is compared on
        // the axis the motion was actually on rather than on whichever
        // component noise happened to win.
        if (mag > 0.05f && g_ser_prev_step > 0.05f) {
            g_ser_flip_denom++;
            float cur = v[g_ser_prev_axis];
            if ((cur < 0) != (g_ser_prev_dom < 0)) {
                g_ser_flip_n++;
                if (mag > g_ser_worst_rev) g_ser_worst_rev = mag;
            }
        }
    }
    g_ser_prev_step = mag;
    g_ser_prev_axis = ax;
    g_ser_prev_dom  = v[ax];
}

static void klovrp_series_report(void) {
    double mean = g_ser_step_n ? g_ser_step_sum / (double)g_ser_step_n : 0.0;
    double alt  = (g_ser_alt_n && mean > 1e-6)
                    ? (g_ser_alt_sum / (double)g_ser_alt_n) / mean : 0.0;
    double flip = g_ser_flip_denom
                    ? 100.0 * (double)g_ser_flip_n / (double)g_ser_flip_denom : 0.0;
    fprintf(stderr,
            "  [ovrp] frozen series: mean step %.4f deg, alternation index "
            "%.3f, sign flips %.0f%% (%llu of %llu moving pairs), worst "
            "reversal %.4f deg (over %llu steps)\n",
            mean, alt, flip,
            (unsigned long long)g_ser_flip_n,
            (unsigned long long)g_ser_flip_denom,
            (double)g_ser_worst_rev,
            (unsigned long long)g_ser_step_n);
    g_ser_step_sum = 0; g_ser_step_n = 0;
    g_ser_alt_sum = 0;  g_ser_alt_n = 0;
    g_ser_flip_n = 0;   g_ser_flip_denom = 0;
    g_ser_worst_rev = 0;
}

static int klovrp_one_pose_get(klovrp_pose *out, int who) {
    if (!klovrp_one_pose_enabled()) return 0;
    // Boot: nothing frozen yet, so the caller keeps its old path. Counted,
    // because "every query fell through" is the reading that would make the
    // rest of this instrument a lie.
    if (!__atomic_load_n(&g_op_valid, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_op_fellthrough, 1, __ATOMIC_RELAXED);
        return 0;
    }
    klovrp_pose h = klovrp_seq_read(&g_op_pose, &g_op_seq);
    klovrp_pose now = klovrp_head_published();
    float d = klovrp_quat_degrees(&h, &now);
    if (d > g_op_div_worst) g_op_div_worst = d;   // racy by design: a statistic
    g_op_div_sum += (double)d;
    __atomic_add_fetch(&g_op_div_n, 1, __ATOMIC_RELAXED);
    if ((unsigned)who < KLOP_N) __atomic_add_fetch(&g_op_served[who], 1, __ATOMIC_RELAXED);
    *out = h;
    return 1;
}

// The frame boundary. Idempotent within a frame: called with the guest's own
// frame index, a second call carrying the same index is a no-op, so it is safe
// to call from every point that could be a frame's first instruction.
//
// `frame_index <= 0` means the guest is not numbering frames in a way we can
// key on (kl_ovrp_frame_latch's OpenXR path, a headless run); those callers
// pass 0 and get an unconditional re-freeze, which is correct because they only
// call it once per frame anyway.
static void klovrp_one_pose_open(int frame_index) {
    if (!klovrp_one_pose_enabled()) return;
    // The frame CYCLE, where the guest gives us one. Spending the
    // arm is a single atomic exchange, so two threads arriving together cannot
    // both freeze: exactly one wins and the other reuses.
    if (klovrp_pose_boundary_cycle() &&
        __atomic_load_n(&g_op_arm_driven, __ATOMIC_ACQUIRE)) {
        if (!__atomic_exchange_n(&g_op_armed, 0, __ATOMIC_ACQ_REL)) {
            __atomic_add_fetch(&g_op_reuses, 1, __ATOMIC_RELAXED);
            return;                               // already frozen for this cycle
        }
    } else if (frame_index > 0 && __atomic_load_n(&g_op_valid, __ATOMIC_ACQUIRE) &&
               frame_index == __atomic_load_n(&g_op_index, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_op_reuses, 1, __ATOMIC_RELAXED);
        return;                                   // already open for this frame
    }
    __atomic_add_fetch(&g_op_freezes, 1, __ATOMIC_RELAXED);

    // klovrp_head_published() rather than the raw read plus the eye-height
    // fixup it used to inline: same two lines, and it is what puts this — the
    // freeze the whole guest frame is served from — behind the head knob's one
    // seam. A frozen pose the knob had not reached would have made the knob a
    // no-op on precisely the path that matters.
    klovrp_pose h = klovrp_head_published();

    uint32_t s = __atomic_load_n(&g_op_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_op_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_op_pose = h;
    __atomic_store_n(&g_op_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_op_index, frame_index, __ATOMIC_RELEASE);
    __atomic_store_n(&g_op_valid, 1, __ATOMIC_RELEASE);
    if (frame_index > 0) __atomic_store_n(&g_op_index_keyed, 1, __ATOMIC_RELEASE);

    // The series this freeze just extended. Here rather than at the publish seam
    // because THIS is the sequence the guest renders: one entry per guest
    // frame, in the order the guest saw them.
    klovrp_series_note(&h);

    // The eye transform in the same breath and at the same instant: a frame
    // whose head is frozen but whose device_from_view still moves under it has
    // simply relocated the intra-frame motion into the stereo pair.
    klovrp_eye_frame_promote();

    static unsigned n;
    if (++n % 300 == 0) {
        double dn = (double)(g_op_div_n ? g_op_div_n : 1);
        fprintf(stderr,
                "  [ovrp] one-pose: served update2 %llu step %llu head %llu "
                "eyeview %llu live %llu | live-divergence worst %.3f deg mean "
                "%.3f | eye-offset rot pushes %llu changes %llu worst %.4f deg "
                "(mode=%s, over 300 frames)\n",
                (unsigned long long)g_op_served[KLOP_UPDATE2],
                (unsigned long long)g_op_served[KLOP_STEP],
                (unsigned long long)g_op_served[KLOP_HEAD],
                (unsigned long long)g_op_served[KLOP_EYEVIEW],
                (unsigned long long)g_op_fellthrough,
                (double)g_op_div_worst, g_op_div_sum / dn,
                (unsigned long long)g_eye_rot_pushes,
                (unsigned long long)g_eye_rot_changes,
                (double)g_eye_rot_worst_deg,
                klovrp_one_pose_enabled() ? "index" : "live");
        // The reading that names the artifact. `poses/frame` is freezes
        // divided by the guest's own frame cycles, and it MUST be 1.00: two
        // freezes in one guest frame means the ~26 pose queries of that frame
        // were answered from two different heads. Index keying read 1.00 in
        // Paint the Town Red's menu (smooth) and 1.70 in gameplay (judder) —
        // the same build, the same eye size, the same warp arm.
        // `update2/freeze` is the same fact from the other side: this guest
        // calls ovrp_Update3 twice a frame, so a healthy freeze serves 2.00.
        {
            uint64_t fz = g_op_freezes, ru = g_op_reuses, ar = g_op_arms;
            double denom = ar ? (double)ar : (double)(fz ? fz : 1);
            fprintf(stderr,
                    "  [ovrp] pose boundary: freezes %llu, reuses %llu, guest "
                    "frame cycles %llu | poses/frame %.2f (MUST be 1.00) | "
                    "update2 per freeze %.2f (this guest calls it 2x a frame) "
                    "(mode=%s)\n",
                    (unsigned long long)fz, (unsigned long long)ru,
                    (unsigned long long)ar,
                    (double)fz / denom,
                    fz ? (double)g_op_served[KLOP_UPDATE2] / (double)fz : 0.0,
                    (klovrp_pose_boundary_cycle() &&
                     __atomic_load_n(&g_op_arm_driven, __ATOMIC_ACQUIRE))
                        ? "cycle" : "index");
            g_op_freezes = 0; g_op_reuses = 0; g_op_arms = 0;
        }
        klovrp_series_report();
        for (int i = 0; i < KLOP_N; i++) g_op_served[i] = 0;
        g_op_fellthrough = 0;
        g_op_div_worst = 0; g_op_div_sum = 0; g_op_div_n = 0;
        g_eye_rot_worst_deg = 0;
    }
}

// Promote the published poses to the ones this guest frame will see. Called at
// the top of the guest's frame, before anything in it can ask.
//
// The number it reports is how far the head moved during the *previous* guest
// frame — i.e. how wrong the recorded pose would be without this latch. At a
// comfortable frame rate it is a fraction of a degree; when the guest is
// struggling it is whole degrees, and a whole degree of mis-correction is
// plainly visible.
void kl_ovrp_frame_latch(void) {
    // Before the enable check: the frame BOUNDARY is where the intra-frame
    // spread window closes whether or not poses are pinned at it, and the
    // measurement is most interesting precisely when they are not.
    klovrp_ifs_frame();
    // ...and this is the one-pose boundary too, but ONLY for a guest that never
    // names its frames. An OVRPlugin guest opens its frame at Update2, which is
    // both earlier and index-keyed; re-freezing here would give that frame two
    // poses. See g_op_index_keyed.
    if (!__atomic_load_n(&g_op_index_keyed, __ATOMIC_ACQUIRE))
        klovrp_one_pose_open(0);
    if (!klovrp_latch_enabled()) return;
    // Through the one seam, so the frame latch carries the knob too. The only
    // difference from the raw read is the eye-height fixup, which klovrp_head()
    // re-applies on the way out of this latch anyway — so it is idempotent.
    klovrp_pose h = klovrp_head_published();
    klovrp_pose l = klovrp_pose_read(&g_hand_pose[0]);
    klovrp_pose r = klovrp_pose_read(&g_hand_pose[1]);

    static float worst, worst_axis[3];
    static unsigned n;
    if (__atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        float moved = klovrp_quat_degrees(&g_frame_head, &h);
        if (moved > worst) worst = moved;
        // ...and the same motion split by AXIS, because "the world swims when I
        // look side to side but not when I nod" has two candidate readings and
        // one number cannot tell them apart. Either the pipeline is wrong about
        // yaw specifically (a geometry fault), or it is wrong in proportion to
        // angular RATE and people simply yaw far faster than they pitch — in
        // which case the asymmetry is latency wearing a geometric disguise and
        // no amount of looking at the yaw axis will find it. These three
        // numbers decide which.
        //
        // The rotation vector of the relative quaternion, which for the
        // fractions of a degree a frame carries is the axis-angle to well
        // inside the reporting precision.
        const klovrp_pose *a = &g_frame_head;
        float cx = -a->qx, cy = -a->qy, cz = -a->qz, cw = a->qw;
        float dw = h.qw*cw - h.qx*cx - h.qy*cy - h.qz*cz;
        float dx = h.qw*cx + h.qx*cw + h.qy*cz - h.qz*cy;
        float dy = h.qw*cy - h.qx*cz + h.qy*cw + h.qz*cx;
        float dz = h.qw*cz + h.qx*cy - h.qy*cx + h.qz*cw;
        if (dw < 0) { dx = -dx; dy = -dy; dz = -dz; }
        const float R2 = 2.0f * (180.0f / 3.14159265358979f);
        float ax[3] = { fabsf(dx) * R2, fabsf(dy) * R2, fabsf(dz) * R2 };
        for (int i = 0; i < 3; i++)
            if (ax[i] > worst_axis[i]) worst_axis[i] = ax[i];
    }
    if (++n % 300 == 0) {
        fprintf(stderr, "  [ovrp] pose latch: worst %.2f deg of head motion "
                        "inside one guest frame over the last 300 "
                        "(pitch %.2f, yaw %.2f, roll %.2f)\n",
                (double)worst, (double)worst_axis[0], (double)worst_axis[1],
                (double)worst_axis[2]);
        worst = 0;
        worst_axis[0] = worst_axis[1] = worst_axis[2] = 0;
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
    // Attributed to this entry point rather than borrowed from klovrp_head(),
    // so the counters say which paths the guest actually asks through.
    klovrp_pose head;
    if (!klovrp_one_pose_get(&head, KLOP_EYEVIEW)) head = klovrp_head();

    float dx, dy, dz, ox, oy, oz, rq[4];
    klovrp_eye_offset(eye, &dx, &dy, &dz);
    klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
    klovrp_pose e = klovrp_eye_cant()
                    ? klovrp_qmul(&head, klovrp_eye_rot(eye, rq)) : head;

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

// The head's own motion, from the sample this frame was latched with — the
// velocity half of "one measurement per frame". A client that wants to predict
// an instant we were not asked about needs it, and OpenXR's XrSpaceVelocity is
// where it asks: without it the only way to move a pose forward is to trust the
// runtime's prediction of the one instant it was told about.
//
// Reads the LATCHED sample, exactly as kl_ovrp_get_guest_head_pose does, so the
// velocity a guest is given belongs to the pose it was given. Returns whether
// those six numbers are a measurement at all — DeviceAnchor publishes no
// velocity, so this is derived from successive poses and the first sample after
// a gap has no basis. Zero is the claim that a moving head is stationary, which
// is the lie OpenXR's velocityFlags == 0 exists to avoid.
int kl_ovrp_head_motion(float *vel, float *ang) {
    klovrp_pose h = klovrp_head();
    if (vel) { vel[0] = h.vx;  vel[1] = h.vy;  vel[2] = h.vz;  }
    if (ang) { ang[0] = h.avx; ang[1] = h.avy; ang[2] = h.avz; }
    return h.motion_valid;
}

// The tag the NEXT kl_ovrp_set_head_pose will carry. Written by the frontend
// immediately before the pose it names, on the frontend's own thread, so the
// pair is as atomic as the seqlock the pose goes through — the guest never sees
// a half-written klovrp_pose, and the tag is inside it.
static uint64_t g_head_tag_next;

void kl_ovrp_set_head_pose_tag(uint64_t tag) {
    __atomic_store_n(&g_head_tag_next, tag, __ATOMIC_RELEASE);
}

// The instant the NEXT head pose is ABOUT — the presentation time the frontend
// asked ARKit to predict the device anchor to. Same publish-order contract as
// the tag above: written on the frontend's own thread immediately before the
// pose it dates. 0 (never set) means "no such clock", and the difference falls
// back to call time. This is the denominator klovrp_derive_motion needs, and
// the reason it needs it is written out there.
// Held as its bit pattern, because the atomic builtins take integers and
// pointers only. A double is exactly 64 bits and the round trip through memcpy
// is exact, so this is a storage detail rather than a conversion.
static uint64_t g_head_time_next;

static double klovrp_head_time_next(void) {
    uint64_t bits = __atomic_load_n(&g_head_time_next, __ATOMIC_ACQUIRE);
    double t; memcpy(&t, &bits, sizeof t);
    return t;
}

void kl_ovrp_set_head_pose_time(double t) {
    uint64_t bits; memcpy(&bits, &t, sizeof bits);
    __atomic_store_n(&g_head_time_next, bits, __ATOMIC_RELEASE);
}

// --- The head velocity kill-switch ------------------------------------------
//
// Once the pose the guest samples, the pose it files, the projection, the warp
// and the present order are all measured clean, what is left inside our
// control that still feeds the picture is the head MOTION we synthesize — the
// guest's own predictor integrates it to extrapolate its render pose, so noise
// in it becomes a per-frame orientation error proportional to head angular
// speed, alternating in sign.
//
// The A/B is to stop synthesizing it. With zeros and motion_valid=1 the guest
// is told "the head is not moving" — which is a false statement about a moving
// head, but a CONSISTENT one, so its predictor extrapolates nothing and its
// render pose is the pose we froze. If the flicker survives that, the derived
// head velocity is not the mechanism.
//
// motion_valid stays 1 deliberately. Answering UNKNOWN instead invites the
// guest to estimate a velocity from the pose series itself, which reintroduces
// the very term being removed and would make a null result unreadable.
//
// HANDS AND CONTROLLERS ARE UNTOUCHED. They publish through
// kl_ovrp_set_hand_motion / kl_ovrp_set_hand_pose and never come through here;
// gun feel, throws and swing detection all read those velocities, and zeroing
// them would break gameplay to test a rendering hypothesis.
//
// Gate: KL_OVRP_ZERO_HEAD_VEL, default 0 (derived velocities). Do not arm it
// in the same run as a change to klovrp_derive_motion: the zeroing masks
// anything done to the velocities. `0` is the higher-fidelity arm.
static int klovrp_zero_head_vel(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_ZERO_HEAD_VEL", 0);
        fprintf(stderr, "  [ovrp] head velocity %s (KL_OVRP_ZERO_HEAD_VEL=%d) — "
                        "the head node reports %s; hands/controllers keep their "
                        "real velocities either way\n",
                on ? "ZEROED" : "derived", on,
                on ? "zero linear and angular velocity, motion_valid=1"
                   : "the finite-differenced velocity from klovrp_derive_motion");
    }
    return on;
}

// What we hand the guest as head motion, per 300 publishes: mean and standard
// deviation of the linear and angular velocity magnitudes. These are taken from
// the DERIVED values before any zeroing, so the instrument keeps reading with
// the kill-switch on — otherwise the arm that matters would report nothing.
//
// The std is the number to look at. A head turning smoothly at a roughly
// constant rate should give a mean well above its own std; a std comparable to
// (or larger than) the mean over a 300-publish window is velocity noise, and
// it should be small now that the dt is the samples' own rather than the
// call's.
static double   g_hv_lin_sum, g_hv_lin_sq, g_hv_ang_sum, g_hv_ang_sq;
static uint64_t g_hv_n;

void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    // DeviceAnchor does not report a velocity, so this derives one. The old
    // note here said the head's motion "is not a field the guest reads for
    // node 9 anyway" — true of an OVRPlugin guest, and node 9 is still served
    // from the same sample. It stopped being the whole story when an OpenXR
    // guest arrived: velocity there is a chained output struct on any space,
    // and zeros in it are an assertion rather than a silence.
    // The tracking-origin rebase, at the one seam every guest-facing pose
    // path already goes through. Doing it here rather than on each of the four
    // read paths (klovrp_head, the step sample, one-pose, the OpenXR spaces)
    // is what makes it impossible to apply twice or to miss one.
    g_raw_head_y = py;
    g_raw_head_have = 1;
    klovrp_origin_capture(py);
    py -= klovrp_origin_y();
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0, 0 };
    v.tag = __atomic_load_n(&g_head_tag_next, __ATOMIC_ACQUIRE);
    klovrp_derive_motion(&v, &g_head_hist, klovrp_head_time_next());

    if (v.motion_valid) {
        double lin = sqrt((double)v.vx*v.vx + (double)v.vy*v.vy + (double)v.vz*v.vz);
        double ang = sqrt((double)v.avx*v.avx + (double)v.avy*v.avy + (double)v.avz*v.avz);
        g_hv_lin_sum += lin; g_hv_lin_sq += lin * lin;
        g_hv_ang_sum += ang; g_hv_ang_sq += ang * ang;
        if (++g_hv_n >= 300) {
            double n  = (double)g_hv_n;
            double lm = g_hv_lin_sum / n, am = g_hv_ang_sum / n;
            double lv = g_hv_lin_sq / n - lm * lm, av = g_hv_ang_sq / n - am * am;
            fprintf(stderr,
                    "  [ovrp] derived motion: ang vel mean %.3f rad/s std %.3f, "
                    "lin vel mean %.3f m/s std %.3f (over %llu publishes, "
                    "handed to guest: %s)\n",
                    am, sqrt(av > 0 ? av : 0), lm, sqrt(lv > 0 ? lv : 0),
                    (unsigned long long)g_hv_n,
                    klovrp_zero_head_vel() ? "ZEROED" : "as derived");
            g_hv_lin_sum = g_hv_lin_sq = g_hv_ang_sum = g_hv_ang_sq = 0;
            g_hv_n = 0;
        }
    }

    // ...and only now, after the instrument has seen the real numbers.
    if (klovrp_zero_head_vel()) {
        v.vx = v.vy = v.vz = v.avx = v.avy = v.avz = 0;
        v.motion_valid = 1;   // see klovrp_zero_head_vel: zero, not unknown
    }

    klovrp_pose_write(&g_head_pose, &v);
    __atomic_store_n(&g_head_set, 1, __ATOMIC_RELEASE);
    // Pose trace, once a second: the pushed head Y + tracking-origin frame
    // (liminal's "floor drops" bug) and the head YAW (missioniss "can't look
    // around" bug — if yaw does not move as the user turns, the rotation we feed
    // the guest is frozen). Yaw from the quaternion, in degrees.
    { static unsigned n; if (n == 0 || n % 90 == 0) {
        float yaw = atan2f(2.0f*(qw*qy + qx*qz), 1.0f - 2.0f*(qy*qy + qz*qz))
                    * (180.0f/3.14159265358979f);
        float pitch = asinf(2.0f*(qw*qx - qy*qz)) * (180.0f/3.14159265358979f);
        fprintf(stderr, "  [ovrp] head Y=%.3f yaw=%.1f pitch=%.1f  origin=%d "
                        "(0=eye,1=floor,2=stage) eye_height=%.3f\n",
                py, yaw, pitch, kl_ovrp_tracking_origin(), kl_ovrp_eye_height());
        n++;
    } else n++; }
}

// The frontend's question — "where is the head NOW" — so it reads the published
// pose, not the one pinned for the guest's frame. The viewer's composite uses
// this as the pose to reproject *towards*, and reprojecting towards the pose
// the picture was already drawn with would be a no-op.
void kl_ovrp_get_head_pose(float *px, float *py, float *pz,
                           float *qx, float *qy, float *qz, float *qw) {
    klovrp_pose h = klovrp_head_published();
    // Back into the FRONTEND's space: the rebase belongs to the guest, and the
    // viewer's composite reprojects against ARKit/its own world, not the
    // guest's origin.
    h.py += klovrp_origin_y();
    if (px) *px = h.px; if (py) *py = h.py; if (pz) *pz = h.pz;
    if (qx) *qx = h.qx; if (qy) *qy = h.qy;
    if (qz) *qz = h.qz; if (qw) *qw = h.qw;
}

// --- ovrp_Update2: the guest's own latch point ---------------------------
//
// OVRPlugin's contract is that tracking is sampled once per frame per *step*:
// the guest calls ovrp_Update2(step, frameIndex, predictionSeconds), and every
// ovrp_GetNodePoseState(step, node) afterwards returns that sample. Measured on
// this title: 85 Update2 calls across 38 frames (two steps a frame) and 760
// GetNodePoseState calls, i.e. twenty reads per frame off two samples.
//
// So the latch is here and not inferred anywhere else — the step, the frame
// index and the moment all come from the guest. Serving the reads from the live
// published pose instead would answer one guest frame with twenty different
// poses, collapse the Render and Physics steps into one drifting value, and
// record for timewarp a pose that was never among the answers given; kl_reproject
// subtracts that record, so the correction would be against a pose that rendered
// nothing.
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

// --- The render pose, keyed by the guest's OWN frame index ------------------
//
// **The bug this exists to remove.** `klovrp_begin_frame_impl` files
// `klovrp_step_head(RENDER)` — the Render-step sample that is live AT THE
// MOMENT BEGINFRAME RUNS. Measured on this title, BeginFrame is issued near
// the END of a nativeRender, with the NEXT frame's first Update2 racing right
// behind it and possibly on a different thread. Lose that race and frame N's
// pixels are filed under frame N+1's pose. Reprojection then subtracts a
// rotation that never happened to those pixels, so the picture is displaced by
// one guest frame of head motion — and because the race is won on some frames
// and lost on others, it is displaced by that amount on SOME frames only.
// Two copies, one guest period of yaw apart, alternating: 13.889 ms at
// ~50 deg/s is 0.7 deg, which is the artifact.
//
// The fix is not a lock and not an ordering assumption — it is to stop asking
// "what is live now" at all. Both `ovrp_Update2` and `ovrp_BeginFrame` are
// handed the ENGINE's own frame index. Update2(Render) files its sample under
// that index; BeginFrame reads back the entry for ITS OWN index. The answer is
// then correct whichever thread got there first, because the two calls agree
// on a name for the frame instead of on a moment.
//
// Threading: every entry is independent and carries its own seqlock, so there
// is no shared cursor for a render thread to contend on. A writer touches one
// slot; a reader touches one slot; nothing orders them but the index.
//
// KL_OVRP_POSE_KEY=index (default) | live. `live` restores the pre-fix
// behaviour for an A/B without a rebuild. The lag histogram is
// printed in BOTH modes, and on the `live` path a nonzero `1x` share is the
// race being caught in the act.
#define KLOVRP_FIDX_RING 32
static struct {
    uint32_t    seq;      // per-entry seqlock; odd = being written
    int         index;    // the guest frame index this entry describes
    int         valid;
    uint64_t    gen;      // the Update2(Render) generation that wrote it
    klovrp_pose head;
} g_fidx[KLOVRP_FIDX_RING];

// Bumped once per Update2(Render). BeginFrame's "lag" is this counter minus the
// generation of the entry its own index maps to: 0 means no other Update2 has
// landed since the one that belongs to this frame, 1 means the next frame's
// sample was already in and the OLD code would have filed it.
static uint64_t g_render_gen;

static int klovrp_pose_by_index(void) {
    static int on = -1;
    if (on < 0) {
        const char *m = kl_env_str("KL_OVRP_POSE_KEY", "index");
        on = !(m && (m[0] == 'l' || m[0] == 'L'));   // "live" turns it off
    }
    return on;
}

// File this frame's Render sample under the guest's name for the frame.
static void klovrp_fidx_store(int frame_index, const klovrp_pose *h) {
    if (frame_index <= 0) return;          // 0/absent: nothing to key on
    unsigned slot = (unsigned)frame_index % KLOVRP_FIDX_RING;
    uint64_t gen = __atomic_add_fetch(&g_render_gen, 1, __ATOMIC_ACQ_REL);
    uint32_t s = __atomic_load_n(&g_fidx[slot].seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_fidx[slot].seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_fidx[slot].index = frame_index;
    g_fidx[slot].gen   = gen;
    g_fidx[slot].head  = *h;
    g_fidx[slot].valid = 1;
    __atomic_store_n(&g_fidx[slot].seq, s + 2, __ATOMIC_RELEASE);
}

// Read back the sample filed under `frame_index`. Returns 1 and fills `out` and
// `lag` on a hit; 0 when this frame has no entry (index 0, a wrapped ring, or a
// guest whose Update2 and BeginFrame do not share a numbering).
static int klovrp_fidx_load(int frame_index, klovrp_pose *out, uint64_t *lag) {
    if (frame_index <= 0) return 0;
    unsigned slot = (unsigned)frame_index % KLOVRP_FIDX_RING;
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(&g_fidx[slot].seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        int   idx = g_fidx[slot].index, ok = g_fidx[slot].valid;
        uint64_t g = g_fidx[slot].gen;
        klovrp_pose h = g_fidx[slot].head;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_fidx[slot].seq, __ATOMIC_RELAXED) != s) continue;
        if (!ok || idx != frame_index) return 0;
        uint64_t now = __atomic_load_n(&g_render_gen, __ATOMIC_ACQUIRE);
        *out = h;
        *lag = now > g ? now - g : 0;
        return 1;
    }
    return 0;
}

static uint64_t klovrp_Update2(int step, int frame_index, double prediction) {
    ovrp_hit("ovrp_Update2");
    { static unsigned n;
      if (n < 48 || n % 600 == 0)
          fprintf(stderr, "  [ovrp] frameloop Update2 #%u step=%d\n", n, step);
      n++; }
    (void)prediction;
    int ix = klovrp_step_ix(step);

    // THE FRAME BOUNDARY, for a guest that speaks OVRPlugin. Whichever step
    // arrives first with a new frame index opens the frame and freezes its one
    // pose; the other step then re-uses it rather than taking a second sample
    // of its own, which is what made Physics and Render disagree by up to three
    // degrees inside one frame. See klovrp_one_pose_open.
    klovrp_one_pose_open(frame_index);

    klovrp_pose h;
    if (!klovrp_one_pose_get(&h, KLOP_UPDATE2)) {
        h = klovrp_head_published();     // the one seam; same two lines inlined
    }
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
    // ...and the same sample under the guest's own name for this frame, which
    // is what BeginFrame will ask for. AFTER the step store, so a BeginFrame
    // that takes the by-index path can never see a keyed entry the step ring
    // does not also carry.
    if (ix == 0) {
        klovrp_fidx_store(frame_index, &h);
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [ovrp] Update2(Render) frame index %d — the key "
                            "BeginFrame will file this frame's pose under\n",
                    frame_index);
        }
    }
    __atomic_store_n(&g_step_valid[ix], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_saw_update2, 1, __ATOMIC_RELEASE);

    // OVRPlugin declares ovrp_Update2 returning ovrpBool, where 0 is false, and
    // both libunity and libil2cpp reference the name — the two conventions
    // collide here. Known gap: 0 is what every measurement so far was taken
    // against, and the correct answer may be OVRP_TRUE.
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
    // One pose for the frame means one pose for BOTH steps: the Render and
    // Physics samples were taken at different instants, and that difference is
    // most of the measured intra-frame spread.
    {
        klovrp_pose op;
        if (klovrp_one_pose_get(&op, KLOP_STEP)) return op;
    }
    int ix = klovrp_step_ix(step);
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_step_head[ix], &g_step_seq);
    return klovrp_head();
}

// Where an Oculus Touch's TRACKED origin sits in its own OpenXR grip frame,
// straight out of the SteamVR render model (tools/rendermodel_frames.py prints
// the table): 10.2 cm back along the handle and pitched 20.6 degrees off it.
// The X term is a point on a hand, so it mirrors between them.
//
// It matters because the two guest-facing XR APIs want DIFFERENT poses out of
// one controller. An OpenXR guest asks for the grip, and the platform publishes
// a grip, so that path is the identity. An OVRPlugin guest is handed the
// tracked pose — the render model is drawn around it — and a title then applies
// its own offset on top, which is why Beat Saber pitches a controller by 40
// degrees to find a saber the model puts 37.4 degrees off tracked.
//
// OFF by default, against that reasoning, because the measurement disagrees
// with it: one shared position offset was found by hand that satisfies the
// OVRPlugin guests and the OpenXR guests at once, and if these two really
// wanted poses 10 cm apart no single value could have. Either a title's own
// offset already assumes the grip, or the guests never use the node pose the
// way the model implies. KL_OVRP_TOUCH_FRAME=1 composes it so one run can
// settle what the argument cannot.
#define KLOVRP_TOUCH_TRACKED_X  ( 0.007000f)
#define KLOVRP_TOUCH_TRACKED_Y  (-0.034157f)
#define KLOVRP_TOUCH_TRACKED_Z  (-0.096073f)
#define KLOVRP_TOUCH_TRACKED_PITCH (-20.60f)

// The residual, measured by playing rather than derived, kept as its own term
// so the two never blur together: everything above is a statement about an
// Oculus Touch that a file can be pointed at, and this is what was left over
// once that statement was applied.
//
// Found on BONELAB with the sense offset at zero, and it is the whole
// correction that target needed after grip-to-tracked landed — where before it
// wanted a chiral 8 cm in X, which nothing in the model could explain. Not
// mirrored, because it did not measure chiral: X came out zero on both hands.
//
// What it probably IS, unresolved: the platform grip and the Touch grip are
// not the same point on a hand. A Sense's tracked origin is 2.1 cm from its
// grip and a Touch's is 10.2 cm from its own, so the two controllers are held
// differently and the frames they call "grip" need not coincide. That would
// make this a per-controller constant rather than a per-guest one, which is
// what it is being treated as.
#define KLOVRP_TOUCH_RESIDUAL_X (0.000000f)
#define KLOVRP_TOUCH_RESIDUAL_Y (0.075000f)
#define KLOVRP_TOUCH_RESIDUAL_Z (0.005000f)

static void klovrp_grip_to_touch(klovrp_pose *p, int hand) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_TOUCH_FRAME", 1);
    if (!on) return;
    // The offset is stated in the grip's own frame, so it is carried by the
    // grip's orientation before the pitch is applied — the pitch turns the
    // reported frame, it does not move the point.
    float mx = hand == 0 ? -1.0f : 1.0f;
    float ox, oy, oz;
    klovrp_qrot(p,
                mx * KLOVRP_TOUCH_TRACKED_X + KLOVRP_TOUCH_RESIDUAL_X,
                KLOVRP_TOUCH_TRACKED_Y + KLOVRP_TOUCH_RESIDUAL_Y,
                KLOVRP_TOUCH_TRACKED_Z + KLOVRP_TOUCH_RESIDUAL_Z, &ox, &oy, &oz);
    p->px += ox; p->py += oy; p->pz += oz;
    float h = KLOVRP_TOUCH_TRACKED_PITCH * 0.5f * 3.14159265358979f / 180.0f;
    float sx = sinf(h), cw = cosf(h);
    // q = q_grip * rotX(pitch), the pitch on the RIGHT because it is expressed
    // in the grip's frame and not in the tracking space's.
    float qx = p->qx, qy = p->qy, qz = p->qz, qw = p->qw;
    p->qx = qw * sx + qx * cw;
    p->qy = qy * cw + qz * sx;
    p->qz = qz * cw - qy * sx;
    p->qw = qw * cw - qx * sx;
}

static klovrp_pose klovrp_step_hand(int step, int hand) {
    int ix = klovrp_step_ix(step);
    klovrp_pose p;
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        p = klovrp_seq_read(&g_step_hand[ix][hand], &g_step_seq);
    else
        p = klovrp_hand(hand);
    // Here and not in klovrp_hand: this is the OVRPlugin guest's read.
    // kl_ovrp_hand_motion is kl_openxr's and must keep the grip.
    klovrp_grip_to_touch(&p, hand);
    return p;
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
// **Sized to kl_glfb's slot count, not to the guest's stage count**, and the
// two are the same key: a frame is filed under the SLOT its picture landed in,
// which on the OpenXR path is allocated per (swapchain, image) and runs well
// past a swapchain's own 0..2 (see kl_glfb.h, kl_glfb_mirror_eye_layer). A ring
// shorter than the slot space silently files nothing for the frames whose slot
// is past its end, which is a compositor with no pose for the picture it is
// holding.
#define KLOVRP_MAX_STAGES 16
static struct {
    kl_ovrp_render_pose r[KLOVRP_MAX_STAGES];
    // The frame between BeginFrame and EndFrame, whose stage is not known yet.
    kl_ovrp_render_pose pending;
    int                 pending_index;   // the GUEST's frame index
    uint64_t            stage_disagree;  // index%%N vs the observed stage
    int                 last_named;      // the stage the previous submit named, or -1
    uint64_t            repeat_drop;     // frames dropped for naming it again
    uint64_t            noeye_drop;      // frames dropped for naming NO eye layer
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
    // How many guest frames have claimed each stage. Stamped into the record at
    // the moment the stage is decided; read live by the compositor to ask
    // whether the texture it is about to sample still holds the record's pixels.
    // See kl_ovrp_render_pose.stage_epoch.
    uint64_t            stage_epoch[KLOVRP_MAX_STAGES];
    pthread_mutex_t     mu;
} g_frames = { .last_complete = -1, .last_named = -1,
               .mu = PTHREAD_MUTEX_INITIALIZER };

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
// 3 rather than 2 because the guest is decoupled from the compositor: one spare buffer only just covers a guest running a frame ahead,
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
// Defined with the controller state it counts, reported from the pose window
// below because that is the one periodic report the guest's frame drives.
static void klovrp_report_controller_reads(void);

// EndFrame. This is where the pose the frame will be rendered with is fixed:
// the frontend wrote it before driving this frame, and every
// ovrp_GetNodePoseState the guest is about to make will answer the same thing.
//
// The argument is the guest's own frame index. The STAGE ring is still indexed
// by our own counter, so a guest that numbers frames differently — or restarts
// them — cannot desynchronise the ring from the records in it. But the POSE is
// now looked up BY that index (klovrp_fidx_store): it is the only thing this
// call and the Update2 that sampled the frame's pose can agree on without
// agreeing on a moment, and agreeing on a moment is precisely what the render
// thread makes impossible.
static uint64_t klovrp_begin_frame_impl(int guest_frame_index) {
// The RENDER step's sample — the pose the guest was handed for this frame
    // and rendered every eye from. Not a fresh read: a fresh read at this
    // moment is a pose the picture was never drawn with, and reprojection
    // subtracts whatever is recorded here.
    klovrp_pose h = __atomic_load_n(&g_saw_update2, __ATOMIC_ACQUIRE)
                    ? klovrp_step_head(KLOVRP_STEP_RENDER) : klovrp_head();
    // ...unless this frame's Update2(Render) filed a sample under this frame's
    // own index, in which case THAT is the pose the pixels embody and the live
    // step sample is only "whatever landed most recently" — see
    // klovrp_fidx_store. The histogram is kept in both modes: on the `live`
    // path a steady nonzero 1x share IS the race, measured.
    {
        klovrp_pose keyed; uint64_t lag = 0;
        int hit = klovrp_fidx_load(guest_frame_index, &keyed, &lag);
        int by_index = hit && klovrp_pose_by_index();
        if (by_index) h = keyed;

        static uint64_t n, n_hit, lag0, lag1, lag2;
        n++;
        if (hit) { n_hit++;
            if (lag == 0) lag0++; else if (lag == 1) lag1++; else lag2++;
        }
        if (n % 300 == 0) {
            double t = (double)n, l = (double)(lag0 + lag1 + lag2);
            uint64_t used = klovrp_pose_by_index() ? n_hit : 0;
            if (l < 1) l = 1;
            fprintf(stderr, "  [ovrp] pose filing: by-index %.0f%% | "
                            "fallback-live %.0f%% | filed-at-lag 0x %.0f%% "
                            "1x %.0f%% 2+x %.0f%% (mode=%s, %llu frames)\n",
                    100.0 * (double)used / t,
                    100.0 * (double)(n - used) / t,
                    100.0 * lag0 / l, 100.0 * lag1 / l, 100.0 * lag2 / l,
                    klovrp_pose_by_index() ? "index" : "live",
                    (unsigned long long)n);
            // ...and, from the same window, whether the guest was still ASKING
            // for controller state at all — see klovrp_report_controller_reads.
            klovrp_report_controller_reads();
            // ...and the eye-stage association state, which is what decides
            // whether these frames get filed at all on a GL guest (kl_glfb.h).
            kl_glfb_eye_map_census(stderr);
            // The two companion numbers from the same window: how far apart the
            // poses handed out inside one guest frame got, and whether the
            // unlatched eye offset moved at all.
            fprintf(stderr, "  [ovrp] intra-frame pose spread: worst %.3f deg | "
                            "eye offset pushes %llu, changes %llu, worst %.3f mm\n",
                    (double)g_ifs_worst_run,
                    (unsigned long long)g_eye_off_pushes,
                    (unsigned long long)g_eye_off_changes,
                    (double)g_eye_off_worst_mm);
            g_ifs_worst_run = 0;
            n = n_hit = lag0 = lag1 = lag2 = 0;
        }
        // The one thing that makes a 100%-fallback reading readable rather than
        // mysterious: say what the two sides were numbering their frames.
        if (!hit) {
            static int said;
            if (!said) { said = 1;
                unsigned slot = (unsigned)(guest_frame_index > 0 ? guest_frame_index : 0)
                                % KLOVRP_FIDX_RING;
                fprintf(stderr, "  [ovrp] BeginFrame(%d) found no keyed pose "
                                "(ring slot holds index %d, valid %d) — filing "
                                "from the live step sample. If this is EVERY "
                                "frame, Update2 and BeginFrame are not sharing "
                                "a frame numbering and KL_OVRP_POSE_KEY cannot "
                                "help.\n",
                        guest_frame_index, g_fidx[slot].index, g_fidx[slot].valid);
            }
        }
    }
    pthread_mutex_lock(&g_frames.mu);
    uint64_t s = ++g_frames.serial;
    // Into the PENDING record, not into a stage. Which stage this frame goes to
    // is the guest's choice and is not knowable yet — it is read off the draw
    // target at EndFrame. Filing it here under a guessed stage is what the old
    // one-stage code got away with and what more than one stage would break.
    kl_ovrp_render_pose *r = &g_frames.pending;
    r->px = h.px; r->py = h.py; r->pz = h.pz;
    r->qx = h.qx; r->qy = h.qy; r->qz = h.qz; r->qw = h.qw;
    // ...and WHICH frontend push this pose came from, so the compositor can
    // find the ARKit DeviceAnchor object it was decomposed from rather than
    // re-deriving one. See kl_ovrp.h `pose_tag` and KL_WARP=system.
    r->pose_tag = h.tag;
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
    r->submit_ns = klovrp_mono_ns();
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
    { static unsigned n; if (n < 48 || n % 300 == 0) {
        uint64_t tid=0; pthread_threadid_np(NULL,&tid);
        fprintf(stderr, "  [ovrp] frameloop BeginFrame4 #%u frameIdx=%d tid=%llu\n",
                n, guest_frame_index, (unsigned long long)tid);
      } n++; }
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
// The eye layer's submitted Fov pair, narrowed to the submitted ViewportRect,
// as eight tangents {l,r,t,b} per eye — the frustum the guest says it ACTUALLY
// rendered, over the part of the texture it says the picture is in. Same reason
// for the forward declaration.
static int klovrp_submit_eye_fov(const void *layer_submits, int count,
                                 const int *vp, const int *of, float tan[8]);
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
// The guest's submitted-frame counter — see KL_OVRP_POKE and kl_vulkan_capture_eyes.
// Declared up here because the frame seam inside klovrp_end_frame_impl is what
// advances it, and KL_OVRP_POKE's `fNNNN` form is timed against the same number.
static unsigned g_vk_frame;

// One read, one place. See the frame seam inside klovrp_end_frame_impl.
static int klovrp_frame_sync_first(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_OVRP_FRAME_SYNC_FIRST", 1);
    return v;
}

// `tangents` is the submitted Fov pair, eight floats in the record's own
// {l,r,t,b} order, or NULL where there is none to read — every guest but a
// Unity XR-SDK one, and a Unity one whose plugin left the field empty. NULL
// keeps what BeginFrame recorded, i.e. the frustum the guest was OFFERED.
static uint64_t klovrp_end_frame_impl(int guest_frame_index, const int *viewports,
                                      const int *viewport_of, int named_stage,
                                      const float *tangents) {
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
    // Measured on device: it happens, and it never happens on
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

    // ...and the same rule for a guest the GL observation cannot see at all.
    //
    // The test above is expressed in draw BINDS, which is a GL measurement, so
    // on a Vulkan guest it is `observed < 0` and the protection above is
    // structurally absent: every frame files a fresh pose, including one that
    // drew nothing, against a stage still holding an older picture. That is the
    // mismatch the paragraph above describes, and it is exactly what a DROPPED
    // frame looks like from here — which is when it is seen.
    //
    // What Vulkan does state is the stage, in the guest's own submit. A guest
    // rotating a three-stage ring names a different one every frame (measured
    // on BONELAB: 85/84/84 across three stages), so naming the SAME stage twice
    // running means it did not rotate — no new picture went anywhere. Filing
    // this frame's pose against it would pair a new pose with the previous
    // frame's image.
    //
    // Deliberately narrow: it needs the observation to be blind AND the guest
    // to have named a stage AND that stage to repeat. A guest that genuinely
    // renders twice into one stage would keep the older pose instead, so this
    // is counted and reported rather than silent, and KL_OVRP_DROP_REPEAT=0
    // turns it off.
    static int drop_repeat = -1;
    if (drop_repeat < 0) drop_repeat = kl_env_on("KL_OVRP_DROP_REPEAT", 1);
    if (!drop && drop_repeat && observed < 0 && named_stage >= 0 &&
        named_stage < stages && named_stage == g_frames.last_named) {
        drop = 1;
        g_frames.repeat_drop++;
    }

    // **...and a frame whose submit named NO eye layer at all drew no picture
    // either** — Into the Radius's loading-screen shake.
    //
    // An Unreal guest stops submitting its eye layer while it compiles shaders,
    // loads a level or shows its splash (klovrp_submit_viewports, the carry);
    // on Into the Radius that was 1720 frames, at ~100 fps, every loading
    // screen. With no stage named and the observation blind, the ladder below
    // fell through to `serial % stages` and filed THIS frame's fresh head pose
    // over whichever stage the counter pointed at — a picture drawn up to three
    // frames earlier at a different head pose, and a different one of the three
    // every frame (the counter disagreed with the guest's own ring on 10240 of
    // 18771 named frames, so it is not even the newest). The compositor then
    // reprojected an old picture by a pose it was never drawn at, rotating
    // through the three stages at the guest's rate: the black strip and its
    // text "shake like crazy" whenever the head moves at all.
    //
    // Dropping the record is the GL path's rule above (a frame that drew into
    // no eye stage files nothing): `last_complete` keeps the last real eye
    // frame with the pose it was drawn at, and the compositor's stall bridge
    // reprojects that one picture correctly — steady, world-held — until the
    // guest submits an eye layer again.
    //
    // Narrow by the same three conditions as the repeat rule: the observation
    // is blind (Vulkan), this submit named no eye stage, and an earlier submit
    // DID name one (so this is a guest that names stages — EndFrame4 — and not
    // the legacy EndFrame, which never does and keeps the counter).
    // KL_OVRP_DROP_NO_EYE=0 files them on the counter again (the A/B).
    static int drop_no_eye = -1;
    if (drop_no_eye < 0) drop_no_eye = kl_env_on("KL_OVRP_DROP_NO_EYE", 1);
    if (!drop && drop_no_eye && observed < 0 && named_stage < 0 &&
        g_frames.last_named >= 0) {
        drop = 1;
        g_frames.noeye_drop++;
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] frame %d submitted NO eye layer — its pose is "
                            "DROPPED rather than filed on a guessed stage over an "
                            "older picture (the loading-screen shake); the last eye "
                            "frame stays on screen with its own pose "
                            "(KL_OVRP_DROP_NO_EYE=0 to go back)\n",
                    guest_frame_index);
        }
    }
    if (named_stage >= 0) g_frames.last_named = named_stage;

    // **THE FRAME SEAM, and it belongs HERE — one statement before the record
    // is filed, not one statement after the call that files it.**
    //
    // The lines below publish `g_frames.r[stage]` and move `last_complete`,
    // which is the moment a compositor is allowed to select this stage and
    // sample its eye texture. On the Vulkan path the ONLY thing ordering that
    // sampling against the guest's own rendering is kl_vulkan_frame_done's
    // vkQueueWaitIdle: KleptonCompositor encodes a GPU wait only on the GL
    // fence, and kl_glfb_gpu_fence_value stays 0 for a guest that never brings
    // ANGLE up — which is precisely what the device's own line "the guest's GPU
    // fence has NOT advanced in 600 composites (stuck at 0)" was reporting. The
    // frame serial the compositor reads alongside it feeds a staleness COUNT
    // and is never a gate.
    //
    // That drain used to run in klovrp_EndFrame4 AFTER this function returned, so between the publish and
    // the drain the stage was both eligible and still being written into. The
    // window is neither theoretical nor small — measured on the host at
    // **5.31 ms mean, 117 ms worst, over 1 ms on 26,186 of 26,237 frames**
    // (Into the Radius, a GPU-timestamp census of the frame seam). A compositor iterating every
    // 8.3 ms lands inside a 5.3 ms window about one time in three, and what it
    // samples there is a PARTIALLY DRAWN frame: the objects whose draws had
    // been issued are present, the ones later in the guest's order are not, and
    // the cut falls somewhere different every time.
    //
    // Which is what Into the Radius showed on the headset — floors, some walls,
    // flowers, fences and wood planks flickering in and out of visibility.
    // Those are drawn at different points in a UE4 frame, so those are the ones
    // that come and go. It is the same failure the GL path had on Tetris Effect
    // (written up below, at the swap gate), reaching the one seam that had no
    // fence.
    //
    // It sits here rather than at the top of klovrp_EndFrame4 so that all the
    // observation work above still overlaps the guest's outstanding GPU work,
    // which is free and is the right shape whatever it measures.
    //
    // **What it costs, measured rather than assumed: nothing.** Consecutive
    // 300 s runs suggested an 8.7% frame-rate loss and that number was WRONG —
    // this host's own throughput drifts by more than that across back-to-back
    // long runs, which is exactly the trap of comparing two numbers taken an
    // hour apart. Interleaved 100 s runs, off/on/off/on: **14,912 / 20,934 /
    // 9,601 / 19,547 frames**. The drain is the same wait either way; only what
    // it is ordered against changes. Interleave the A/B if it is ever
    // re-measured.
    //
    // **KL_OVRP_FRAME_SYNC_FIRST=0 restores the old order**, so the A/B is one
    // variable, and on a guest that is not Vulkan this is not on the path at
    // all.
    if (klovrp_frame_sync_first() && kl_vulkan_guest_active()) {
        static int said;
        if (!said++)
            fprintf(stderr,
                    "  [ovrp] the guest's queue is drained BEFORE the frame "
                    "record is filed — a compositor cannot select a stage the "
                    "GPU is still writing into "
                    "(KL_OVRP_FRAME_SYNC_FIRST=0 goes back)\n");
        // The capture stays on the drain's own side of the seam and ahead of
        // it, exactly as it was: its readback is work on the same queue, so a
        // compositor let loose between the two would race it.
        kl_vulkan_capture_eyes(g_vk_frame++, named_stage < 0 ? 0 : named_stage);
        kl_vulkan_frame_done(named_stage < 0 ? 0 : named_stage);
    }
    pthread_mutex_lock(&g_frames.mu);
    // Before the record is filed, and into the same record: the viewport
    // describes THIS frame's picture exactly as the pose and the tangents do,
    // and a compositor that reads one from the stage ring and the other from a
    // global would crop frame N's picture by frame N+1's rect on any run where
    // the guest changes it — which is the moment this exists for.
    if (viewports) memcpy(g_frames.pending.viewport, viewports, sizeof g_frames.pending.viewport);
    if (viewport_of) memcpy(g_frames.pending.viewport_of, viewport_of,
                            sizeof g_frames.pending.viewport_of);
    // And the frustum: BeginFrame recorded what the guest was offered, and this
    // is the guest's own correction at the bottom of the frame. A composite
    // that placed the picture with the offered cone while the guest rendered
    // the union of the two is the "everything is doubled" failure (GORN 2) —
    // see ovrp_layer_submit_eyefov.
    if (tangents) memcpy(g_frames.pending.tangents, tangents,
                         sizeof g_frames.pending.tangents);
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
        // The stage's identity, claimed here because THIS is the moment the
        // guest's pixels take possession of that texture. Stamped before the
        // record is filed so a compositor comparing it against the live count
        // learns that the picture has since been overwritten by a later guest
        // frame.
        g_frames.pending.stage_epoch = ++g_frames.stage_epoch[stage];
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
    // KL_GLFB_OUT is otherwise silently inert on the one path that has pixels.
    // kl_openxr.c's xrEndFrame carries the identical block for the identical
    // reason.
    //
    // Gated on whether the guest is STILL swapping, not on whether it ever
    // swapped once — and the difference is a whole port's worth of tearing.
    //
    // 1.28's legacy VRDevice calls BOTH this and eglSwapBuffers, and presenting
    // twice a frame would double every capture and hand a frontend two frames
    // per frame, so a guest whose swap IS its presentation signal must not be
    // presented from here. "Has it ever swapped" answers a different question,
    // and UE4 is the guest that separates the two: Tetris Effect calls
    // eglSwapBuffers exactly ONCE, during EGL bring-up, and never again — its
    // census reads `eglSwapBuffers 1` beside 10,432 EndFrame4s. From that
    // one swap onward this block was dead, and with it went the GPU FENCE.
    //
    // That is not a lost screenshot. `kl_glfb_present` is what encodes "the
    // guest's frame is done" into ANGLE's command stream (klfb_gpu_frame_now),
    // and the compositor waits on the value it publishes before sampling the
    // eye texture. With the value frozen at 1 the wait is satisfied for ever,
    // and the composite reads the eye texture with NO ordering against the
    // guest's own rendering into it — for the whole run. Three stages and a
    // guest at the panel's own rate is exactly the case where that bites: the
    // phase between the two drifts, and while it lines up the composite samples
    // a stage the guest is part-way through redrawing. What reaches the panel
    // is then part frame N and part frame N+3, with the seam in a different
    // place every present. That reads as **two images alternating**, in either
    // eye alone — "duplicates flash back and forth", as it was reported from
    // the headset.
    //
    // It is invisible to every instrument in this hunt by construction: the
    // record, the serial, the stage, the pose and the anchor all describe the
    // frame correctly. They just do not describe the PIXELS.
    //
    // The compositor's own `stale in a row` said so all along and was read as
    // an instrument fault: it counts display periods whose guest-frame value
    // has not moved, and it reached 10,767 of 10,857 iterations while the stage
    // serials climbed to 10,284. A frozen fence value is exactly that.
    //
    // So: this EndFrame is the presentation signal unless a swap has happened
    // SINCE THE LAST ONE. A guest that swaps every frame still defers; a guest
    // that swapped once at boot does not lose the fence for the rest of its
    // life.
    static unsigned long swaps_at_last_endframe;
    unsigned long swaps_now = (unsigned long)kl_egl_swap_count();
    int guest_swaps_its_frames = (swaps_now != swaps_at_last_endframe);
    swaps_at_last_endframe = swaps_now;
    if (!guest_swaps_its_frames) {
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
    return klovrp_end_frame_impl(guest_frame_index, NULL, NULL, -1, NULL);
}

    // ovrp_EndFrame4(frameIndex, layerSubmits, layerSubmitCount, sync) — the 1.40
// shape (real 0x16ed40, which -1001s only when layerSubmits is NULL *and* the
// count is non-zero, so an empty submission is legal). The layer list is the
// guest naming which layers it just filled. The STAGE does not come from that
// list: it comes from the observation window BeginFrame opened, which is the
// stronger statement of the two (it is what the GL side actually saw).
//
// It is read for one thing, and it is the only place that thing is stated: the
// per-eye **ViewportRect**, i.e. how much of the eye texture the guest actually
// drew into this frame. See kl_ovrp_render_pose.viewport — a title lowering its
// render resolution does it here, not by resizing anything.
//
// On the XR-SDK path `frameIndex % stages` is NOT the stage, and the report
// says so: a 9000-frame 1.40 run counts 8786 "the guest's frame index
// disagreed". That is not a fault — the display provider rotates its own
// TextureStage ring and the index it passes here has no relation to it, where
// 1.28's legacy VRDevice derives the stage from exactly this counter (0
// disagreements). The observation is authoritative either way — the same run
// reports 0 frames drawn into no stage, 0 into several and 0 off-thread — so the
// counter is only ever the fallback for runs with no GL observation at all.

static uint64_t klovrp_EndFrame4(int guest_frame_index, const void *layer_submits,
                                 int layer_submit_count, void *sync) {
    ovrp_hit("ovrp_EndFrame4");
    { static unsigned n; if (n < 48 || n % 300 == 0) {
        uint64_t tid=0; pthread_threadid_np(NULL,&tid);
        fprintf(stderr, "  [ovrp] frameloop EndFrame4 #%u frameIdx=%d layers=%d "
                "sync=%p tid=%llu\n",
                n, guest_frame_index, layer_submit_count, sync,
                (unsigned long long)tid);
        // Phase Sync: AC Nexus logs "Phase sync hack is on", and under it the
        // NEXT frame's start can wait on state this call is expected to service.
        // Dump the object's first words once so the wait, if that is what it
        // is, has a shape we can identify.
        if (sync && n == 0) {
            const uint64_t *w = (const uint64_t *)sync;
            for (int i = 0; i < 8; i++)
                fprintf(stderr, "  [ovrp]   sync[+%2d] %#018llx\n", i*8,
                        (unsigned long long)w[i]);
        }
      } n++; }
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

    // **The guest's GPU work is DRAINED BEFORE the record is filed, not after.**
    //
    // klovrp_end_frame_impl is what publishes `g_frames.r[stage]` and moves
    // `last_complete` — i.e. it is the moment a compositor is allowed to select
    // this stage and sample its eye texture. On the Vulkan path the ONLY thing
    // ordering that sampling against the guest's own rendering is
    // kl_vulkan_frame_done's vkQueueWaitIdle: the compositor encodes no GPU
    // wait at all here (KleptonCompositor's wait is on the GL fence, and
    // kl_glfb_gpu_fence_value stays 0 for a guest that never brings ANGLE up —
    // which is what the device's own "the guest's GPU fence has NOT advanced in
    // 600 composites, stuck at 0" line was reporting), and the frame serial it
    // reads is used for a staleness COUNT and never as a gate.
    //
    // The drain used to run AFTER the publish, so between the two the
    // stage was both eligible and still being written. That window is not
    // theoretical and it is not small: measured on the host at
    // **5.31 ms mean, 117 ms worst, and over 1 ms on 26,186 of 26,237 frames**
    // (Into the Radius, a GPU-timestamp census of the frame seam). A compositor
    // iterating every 8.3 ms lands inside a 5.3 ms window roughly one time in
    // three, and what it samples then is a PARTIALLY DRAWN frame — the objects
    // whose draws had been issued are there and the ones later in the guest's
    // order are not, with a different cut every time.
    //
    // Which is exactly what Into the Radius showed: floors, some walls, flowers,
    // fences and wood planks flickering in and out of visibility. Those are
    // drawn at different points in a UE4 frame, so those are the things that
    // come and go. It is the same failure the GL path had on Tetris Effect —
    // written up further down this file — arriving through the one seam that
    // had no fence.
    //
    // KL_OVRP_FRAME_SYNC_FIRST=0 restores the old order, so the A/B is one
    // variable. The cost of the new one is that the record is filed a few
    // milliseconds later, i.e. the compositor holds the previous frame slightly
    // longer, which is the case reprojection exists for.
    // ...and the frustum it rendered with, a separate statement from the rect
    // and the one that matters under Oculus symmetric projection: the rect
    // stays full width there (OVRPlugin 1.106) and only Fov[] moves.
    float tan[8];
    int have_fov = klovrp_submit_eye_fov(layer_submits, layer_submit_count,
                                         have ? vp : NULL, have ? of : NULL, tan);
    uint64_t r = klovrp_end_frame_impl(guest_frame_index, have ? vp : NULL,
                                       have ? of : NULL, named,
                                       have_fov ? tan : NULL);

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
    if (!klovrp_frame_sync_first() && kl_vulkan_guest_active()) {
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

// The pair above, but as ONE selection. See the header: two calls are two
// instants, and the committer runs on the GPU fence thread between them.
int kl_ovrp_last_complete_record(kl_ovrp_render_pose *out) {
    if (!out) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int s = g_frames.last_complete;
    int have = s >= 0 && s < KLOVRP_MAX_STAGES && g_frames.r[s].serial != 0;
    if (have) *out = g_frames.r[s];
    pthread_mutex_unlock(&g_frames.mu);
    return have;
}

uint64_t kl_ovrp_stage_epoch_now(int stage) {
    if ((unsigned)stage >= KLOVRP_MAX_STAGES) return 0;
    pthread_mutex_lock(&g_frames.mu);
    uint64_t e = g_frames.stage_epoch[stage];
    pthread_mutex_unlock(&g_frames.mu);
    return e;
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
    r->pose_tag = h.tag;
    // Through klovrp_eye_tan, NOT g_eye_tan: this is what the composite
    // builds its quad from, so it has to be the frustum the guest was
    // actually told to render with, unified or not.
    for (int e = 0; e < 2; e++)
        memcpy(r->tangents[e], klovrp_eye_tan(e), sizeof r->tangents[e]);
    r->serial = s;
    r->stage = -1;
    r->complete = 0;
    r->submit_ns = klovrp_mono_ns();
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
        // The stage's identity, claimed here because THIS is the moment the
        // guest's pixels take possession of that texture. Stamped before the
        // record is filed so a compositor comparing it against the live count
        // learns that the picture has since been overwritten by a later guest
        // frame.
        g_frames.pending.stage_epoch = ++g_frames.stage_epoch[stage];
        g_frames.r[stage] = g_frames.pending;
        g_frames.last_complete = stage;
        g_frames.filed[stage]++;
    }
    pthread_mutex_unlock(&g_frames.mu);
}

// ---- the two hands ---------------------------------------------------------
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

// ---- what each path actually applies to a controller pose -----------------
//
// The controller correction is spread across three seams and no log joined
// them, which is why "the alignment is wrong on some targets" could not say
// WHERE. The frontend applies a convention offset when it publishes
// (KleptonControllers' KLSenseTune: a hilt pitch and a position nudge, tuned by
// playing Beat Saber); kl_ovrp passes that through to an OVRPlugin guest
// unchanged; kl_openxr adds its own grip pitch to a LOCAL copy for an OpenXR
// guest. So two guests can receive rotations that differ by tens of degrees
// with every constant behaving exactly as documented.
//
// This prints what is PUBLISHED. kl_openxr prints what it hands its guest after
// its own pitch, in the same shape and the same units, so one device log
// carries both numbers and the difference between them is the answer.
//
// Once per hand, then only when it moves materially, and never more than once
// every two seconds: a controller pose changes every frame and a per-frame line
// would be the log.
static void klovrp_pose_euler_deg(const klovrp_pose *p, float *out) {
    float x = p->qx, y = p->qy, z = p->qz, w = p->qw;
    const float R = 57.29577951308232f;
    // XYZ order, matching the frontend's klEulerXYZ so the two can be compared
    // term by term rather than only in magnitude.
    float sx = 2.0f * (w * x + y * z), cx = 1.0f - 2.0f * (x * x + y * y);
    float sy = 2.0f * (w * y - z * x);
    float sz = 2.0f * (w * z + x * y), cz = 1.0f - 2.0f * (y * y + z * z);
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    out[0] = atan2f(sx, cx) * R;
    out[1] = asinf(sy) * R;
    out[2] = atan2f(sz, cz) * R;
}

void kl_ovrp_ctrl_trace(const char *where, int hand, const float *euler_deg,
                        const float *pos) {
    static int on = -1;
    // Opt-in: for an OVRPlugin guest this says nothing the frontend's own
    // `[cp] Sense grip pitch` line does not, and it prints while the user is
    // playing. It earns its place on the OpenXR path, where the two lines
    // together are the only statement of what that path adds.
    if (on < 0) on = kl_env_on("KL_CTRL_TRACE", 0);
    if (!on || (unsigned)hand > 1) return;
    // Keyed on the SOURCE as well as the hand, so the two seams do not silence
    // each other — they describe different numbers and both are wanted.
    static struct { const char *w; int hand; float e[3]; uint64_t t; } said[4];
    static int nsaid;
    int slot = -1;
    for (int i = 0; i < nsaid; i++)
        if (said[i].w == where && said[i].hand == hand) { slot = i; break; }
    if (slot < 0) {
        if (nsaid >= (int)(sizeof said / sizeof said[0])) return;
        slot = nsaid++;
        said[slot].w = where; said[slot].hand = hand;
        said[slot].t = 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    float moved = 0;
    for (int i = 0; i < 3; i++) {
        float d = euler_deg[i] - said[slot].e[i];
        if (d < 0) d = -d;
        if (d > moved) moved = d;
    }
    if (said[slot].t && (moved < 2.0f || now - said[slot].t < 2000000000ull)) return;
    said[slot].t = now;
    for (int i = 0; i < 3; i++) said[slot].e[i] = euler_deg[i];
    fprintf(stderr, "  [ctrl] %s hand %d: euler XYZ %.1f %.1f %.1f deg",
            where, hand, (double)euler_deg[0], (double)euler_deg[1],
            (double)euler_deg[2]);
    if (pos)
        fprintf(stderr, ", pos %.3f %.3f %.3f m",
                (double)pos[0], (double)pos[1], (double)pos[2]);
    fprintf(stderr, "\n");
}

static kl_ovrp_head_at_fn g_head_at;

void kl_ovrp_set_head_at(kl_ovrp_head_at_fn fn) {
    __atomic_store_n(&g_head_at, fn, __ATOMIC_RELEASE);
}

int kl_ovrp_head_predict_delta(double time_s, float *quat4, float *pos3) {
    kl_ovrp_head_at_fn fn = __atomic_load_n(&g_head_at, __ATOMIC_ACQUIRE);
    if (!fn) return 0;
    float w[7];
    if (!fn(time_s, w)) return 0;
    // delta = predicted o current^-1, so applying it on the left of anything
    // built from the current pose moves it to where the prediction says.
    klovrp_pose h = klovrp_head();
    float ix = -h.qx, iy = -h.qy, iz = -h.qz, iw = h.qw;   // conjugate = inverse
    // q_delta = q_pred * q_cur^-1
    quat4[0] = w[3] * ix + w[0] * iw + w[1] * iz - w[2] * iy;
    quat4[1] = w[3] * iy - w[0] * iz + w[1] * iw + w[2] * ix;
    quat4[2] = w[3] * iz + w[0] * iy - w[1] * ix + w[2] * iw;
    quat4[3] = w[3] * iw - w[0] * ix - w[1] * iy - w[2] * iz;
    // p_delta = p_pred - q_delta * p_cur
    klovrp_pose d;
    d.qx = quat4[0]; d.qy = quat4[1]; d.qz = quat4[2]; d.qw = quat4[3];
    float rx, ry, rz;
    klovrp_qrot(&d, h.px, h.py, h.pz, &rx, &ry, &rz);
    pos3[0] = w[4] - rx;
    pos3[1] = w[5] - ry;
    pos3[2] = w[6] - rz;
    return 1;
}

void kl_ovrp_set_hand_motion(int hand, float px, float py, float pz,
                             float qx, float qy, float qz, float qw,
                             float vx, float vy, float vz,
                             float avx, float avy, float avz) {
    if ((unsigned)hand > 1) return;
    py -= klovrp_origin_y();          // the same space the head is filed in
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, vx, vy, vz, avx, avy, avz, 1, 0 };
// A measured velocity is authoritative — nothing is derived here. The
    // history still advances, so a publisher that later drops to the pose-only
    // call differentiates against the right previous sample instead of a gap.
    klovrp_hist_note(&g_hand_hist[hand], &v, 0);
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
    float e[3]; klovrp_pose_euler_deg(&v, e);
    float pp[3] = { px, py, pz };
    kl_ovrp_ctrl_trace("published", hand, e, pp);
}

// ...and the pose-only form, which is what the macOS viewer publishes. It
// differentiates rather than passing six zeros to the call above — zeros are the
// claim that a moving controller is stationary (see klovrp_pose.motion_valid).
//
// Presence, as every read side should answer it. KL_OVRP_HANDS_IN_VIEW parks
// the hands in the frustum — but the park alone never flipped this bit, so an
// OpenXR guest (whose controller existence IS this flag, via
// kl_ovrp_controller_input) saw "untracked" forever and the knob's own
// question — does the guest draw controllers at all — was unanswerable on
// that door. A hand the knob parks in view is a hand that exists: presence
// follows the knob.
static int klovrp_hand_present(int hand) {
    static int inview = -1;
    if (inview < 0) inview = kl_env_on("KL_OVRP_HANDS_IN_VIEW", 0);
    if (inview) return 1;
    return __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE) ? 1 : 0;
}
void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    if ((unsigned)hand > 1) return;
    py -= klovrp_origin_y();          // the same space the head is filed in
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0, 0 };
    klovrp_derive_motion(&v, &g_hand_hist[hand], 0);
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
    float e[3]; klovrp_pose_euler_deg(&v, e);
    float pp[3] = { px, py, pz };
    kl_ovrp_ctrl_trace("published", hand, e, pp);
}

// Is the velocity in the latched hand sample a measurement, or is it the
// "we do not know" that OpenXR spells velocityFlags == 0?
int kl_ovrp_hand_motion_known(int hand) {
    if ((unsigned)hand > 1) return 0;
    return g_hand_pose[hand].motion_valid;
}

// Has a frontend ever published a head pose / this hand's pose? See kl_ovrp.h.
// These read the same two sticky flags the pose seam sets, and nothing else —
// no latch, no one-pose slot, no counter — because the boot gate polls them in
// a loop and a readiness probe that moved the instruments would be measuring
// itself.
int kl_ovrp_head_pose_published(void) {
    return __atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int kl_ovrp_hand_pose_published(int hand) {
    if ((unsigned)hand > 1) return 0;
    return klovrp_hand_present(hand);
}

void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y) {
    if ((unsigned)hand > 1) return;
    // Bounded change-log: every button-word TRANSITION, so "I pressed A and
    // nothing happened" splits into "the bit never arrived" (frontend mapping)
    // vs "it arrived and the guest ignored it" (guest-side read). Always on -
    // presses are rare, so this is a handful of lines per session.
    { static uint32_t last[2] = { 0xffffffffu, 0xffffffffu };
      static unsigned n;
      if (buttons != last[hand] && n < 400) { n++;
          fprintf(stderr, "  [input] hand %d buttons %#x -> %#x (trig %.2f grip %.2f)\n",
                  hand, last[hand] == 0xffffffffu ? 0 : last[hand], buttons,
                  (double)index_trigger, (double)hand_trigger);
          last[hand] = buttons;
      } }
    g_input[hand].buttons = buttons;
    g_input[hand].touches = touches;
    g_input[hand].neartouches = touches;   // capacitive proximity ~ touch
    g_input[hand].index_trigger = index_trigger;
    g_input[hand].hand_trigger = hand_trigger;
    g_input[hand].stick_x = stick_x;
    g_input[hand].stick_y = stick_y;

    // RIGHT Options button (PSVR2 Sense) -> a console command, on the press edge.
    // Reaches us as the BACK bit on the right hand (system/click). Default (hl1)
    // skips past the broken intro to the first real level, c1a0 (Anomalous
    // Materials): the intro guard's Sector C door is stuck (its scripted chain
    // never fires on the seamless transition) and this build has no working escape
    // from it — ent_fire is unregistered and noclip isn't honored by the VR
    // locomotion; changelevel is. KL_XASH_GRIP_CMD overrides the whole command.
    if (hand == 1) {
        static int prev = 0;
        int now = (buttons & KL_OVRP_RAW_BACK) ? 1 : 0;
        if (now && !prev) {
            extern const char *kl_driver_target_name(void);
            extern void        kl_xash_console_cmd(const char *);
            const char *t = kl_driver_target_name();
            const char *cmd = getenv("KL_XASH_GRIP_CMD");
            if (!cmd && t && !strcmp(t, "hl1")) cmd = "changelevel c1a0\n";
            if (cmd) kl_xash_console_cmd(cmd);
        }
        prev = now;
    }
    // LEFT Create button (menu/click -> START, left hand): open/close the game menu.
    // The guest's own START->menu path isn't surfacing the menu here, so inject
    // `escape` (CL_Escape_f toggles it). KL_XASH_MENU_CMD overrides.
    if (hand == 0) {
        static int mprev = 0;
        int mnow = (buttons & KL_OVRP_RAW_START) ? 1 : 0;
        if (mnow && !mprev) {
            extern const char *kl_driver_target_name(void);
            extern void        kl_xash_console_cmd(const char *);
            const char *t = kl_driver_target_name();
            const char *cmd = getenv("KL_XASH_MENU_CMD");
            if (!cmd && t && !strcmp(t, "hl1")) cmd = "escape\n";
            if (cmd) kl_xash_console_cmd(cmd);
        }
        mprev = mnow;
    }
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

            // ---...and the read side, for kl_openxr.c (kl_ovrp.h documents the contract)
//
// The pose comes from klovrp_hand(), which is the PINNED one — the same value
// ovrp_GetNodePoseState hands the other guest in the same frame. Reading
// g_hand_pose directly here would have been the shorter line and would have
// reintroduced the unlatched read in a new API.
int kl_ovrp_hand_motion(int hand, float *pos, float *quat, float *vel, float *ang) {
    if ((unsigned)hand > 1) return 0;
    klovrp_pose p = klovrp_hand(hand);
    // KL_OVRP_HANDS_IN_VIEW=1 for a guest that reads its hands HERE (an
    // OpenXR guest's action spaces, kl_openxr) rather than through
    // ovrp_GetNodePoseState: the same head-relative park. Before this the knob
    // asserted presence on this path and left the pose at the origin (Half-Life:
    // Alyx read its stage-space hands at 0,0,0 — on the floor, with a menu ray
    // that hits nothing).
    static int inview = -1;
    if (inview < 0) inview = kl_env_on("KL_OVRP_HANDS_IN_VIEW", 0);
    if (inview) {
        klovrp_pose head = klovrp_head();
        float ox, oy, oz;
        klovrp_qrot(&head, hand ? 0.15f : -0.15f, -0.12f, -0.55f, &ox, &oy, &oz);
        klovrp_pose park = head;
        park.px = head.px + ox; park.py = head.py + oy; park.pz = head.pz + oz;
        park.vx = park.vy = park.vz = park.avx = park.avy = park.avz = 0.0f;
        p = park;
    }
    if (pos)  { pos[0] = p.px; pos[1] = p.py; pos[2] = p.pz; }
    if (quat) { quat[0] = p.qx; quat[1] = p.qy; quat[2] = p.qz; quat[3] = p.qw; }
    if (vel)  { vel[0] = p.vx; vel[1] = p.vy; vel[2] = p.vz; }
    if (ang)  { ang[0] = p.avx; ang[1] = p.avy; ang[2] = p.avz; }
    return klovrp_hand_present(hand);
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
    return klovrp_hand_present(hand);
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
    // The 0x127a000+1728 offset is HARDCODED for one specific libunity build. On
    // any other build it reads a garbage `obj`, and walking it faults (this is
    // exactly what crashed AC Nexus when KL_FULL wrongly armed this diagnostic).
    // So probe every read through vm_read_overwrite, which returns an error rather
    // than faulting on an unmapped address — the diagnostic degrades to a one-line
    // "not this build" note instead of taking the process down.
    uint32_t hdr[3];
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)obj, sizeof hdr,
                          (vm_address_t)hdr, &got) != KERN_SUCCESS || got != sizeof hdr) {
        fprintf(stderr, "  [ovrp] VRDevice %p: unreadable — not this libunity build\n", obj);
        return;
    }
    fprintf(stderr, "  [ovrp] VRDevice %p: idLeft=%u idRight=%u idHmd=%u\n",
            obj, hdr[0], hdr[1], hdr[2]);
// Name every function-pointer slot by matching it against what we handed
    // back from kl_ovrp_sym. This is the VRDevice's whole contract with the
    // plugin in one place: which entry point sits behind each `ldr x8, [x?,
    // #N] / blr x8` in the disassembly, so a status predicate we answer wrong
    // can be found by name instead of by chasing offsets.
    void *slots[768 / 8];
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)obj, sizeof slots,
                          (vm_address_t)slots, &got) != KERN_SUCCESS || got != sizeof slots)
        return;
    for (size_t off = 0; off + 8 <= 768; off += 8) {
        void *fn = slots[off / 8];
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
    klovrp_ifs_observe(&head);
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
        float rq[4];
        eye = klovrp_eye_cant()
              ? klovrp_qmul(&head, klovrp_eye_rot(node, rq)) : head;
        eye.px = head.px + ox; eye.py = head.py + oy; eye.pz = head.pz + oz;
        p = &eye;
    } else if (node == 3 || node == 4 || node == 12 || node == 13) {
        // HandLeft/HandRight (3/4) AND ControllerLeft/ControllerRight (12/13).
        // Both name the same physical thing on this platform: the controller we
        // synthesise from hand tracking (or a Sense accessory). A Unity title
        // reads its controller pose from the HAND nodes; UE5's Meta XR plugin
        // (olar) reads it from the CONTROLLER nodes. When only 3/4 were mapped,
        // node 12/13 fell through to `p = &head` below — so olar's controllers
        // were pinned to the head pose and moved only when the head moved,
        // while the aim/select the guest built on them never tracked the hands.
        int h = (node == 3 || node == 12) ? 0 : 1;
        // KL_OVRP_HANDS_IN_VIEW=1: park both hands at a fixed spot well inside
        // the *current head's* frustum, overriding whatever the frontend last
        // wrote. Answers one question and only one — does the guest draw
        // controllers at all? Read side, so it beats the viewer's per-frame
        // writes. Diagnostic: the hands do not move with your real hands.
        //
        // Head-relative, not absolute tracking space: the guest asks for a
        // FloorLevel origin, so coordinates chosen for an identity head park
        // the hands 12 cm below the floor and 1.7 m under the real head —
        // reliably out of frustum, i.e. hidden by the knob meant to show them.
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
    // The floor offset (klovrp_floor_offset — the argument is up there, in
    // full). THIS line is the whole seam: every node the guest asks about —
    // both eyes, EyeCenter, both hands, Head — comes out through it, and
    // nothing else in this file writes a position into a guest struct. Keeping
    // it here rather than in the stored pose is what leaves the recorded render
    // pose in ARKit space, where KleptonCompositor's reprojection needs it.
    float floor_dy = klovrp_floor_offset();
    if (floor_dy != 0.0f) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr,
                    "  [ovrp] tracking origin is %d (not EyeLevel) and a real "
                    "tracker is publishing, so node positions are raised %.3f m "
                    "to put y=0 on the FLOOR (KL_OVRP_FLOOR_OFFSET). ARKit's "
                    "world origin sits at the device's own start pose, i.e. at "
                    "eye level, not on the ground.\n",
                    g_tracking_origin, (double)floor_dy);
        }
    }
    float *f = out;
    f[0] = p->qx; f[1] = p->qy;      // quat xyz at +0x00
    f[2] = p->qz; f[3] = p->qw;      // quat w at +0x0c
    f[4] = p->px; f[5] = p->py + floor_dy;   // position at +0x10
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

static uint64_t klovrp_WaitToBeginFrame(long frame_index) {
    ovrp_hit("ovrp_WaitToBeginFrame");
    // THE GUEST'S FRAME CYCLE, and the only boundary both of its threads
    // agree on. Called exactly once per frame — 1307 calls over 1307 frames
    // on Paint the Town Red VR — and before the pacer, because a guest that
    // blocks here must still have opened its cycle. See klovrp_one_pose_arm.
    klovrp_one_pose_arm();
    { static unsigned n; if (n < 48 || n % 300 == 0) {
        uint64_t tid=0; pthread_threadid_np(NULL,&tid);
        fprintf(stderr, "  [ovrp] frameloop WaitToBeginFrame #%u frameIdx=%ld tid=%llu\n",
                n, frame_index, (unsigned long long)tid);
      } n++; }
    if (g_frame_pacer) {
        g_frame_pacer();
// Here rather than at BeginFrame, and for the reason kl_ovrp_frame_latch
        // gives: the pose has to be pinned before ANYTHING in the frame can ask
        // for one, and this call is the frame's first instruction. A guest that
        // asks between the two would see the compositor's newer pose and draw
        // from a pose the record does not carry.
        kl_ovrp_frame_latch();
    }
    // The CAPI return is an ovrpResult, not a frame id — the frame index is the
    // CALLER's input. Returning a counter here read as success only by luck
    // (positive ovrpResults are success variants); say success plainly.
    return OVRP_SUCCESS;
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

// --- is the guest still ASKING? ------------------------------------------
//
// When every button stops working after a glance at a notification, three
// stories fit equally well and no line separated them: the host stopped
// reading the controllers, the host kept reading but we stopped publishing, or
// the guest stopped polling. The host half is instrumented on the Swift side
// (the empty-queue fallback, and one line per button element the first time it
// is pressed). This is the other end: how many times
// the guest asked us for controller state in the last window, and whether what
// we handed back had anything down in it.
//
// A window with `guest polled 0` says the guest's input door closed — UE's own
// focus gate, and nothing on the host side would ever fix it. A window with
// thousands of polls and a nonzero button mask says the guest was handed the
// press and did something else with it, which is what a focused text widget
// eating input looks like. Two numbers, three stories, one of them left.
static uint64_t g_ctrl_reads, g_ctrl_reads_down;
static uint32_t g_ctrl_last_buttons[2];
static float    g_ctrl_last_trigger[2];

static void klovrp_report_controller_reads(void) {
    fprintf(stderr, "  [ovrp] controller reads: guest polled %llu times, "
                    "%llu with something down | last state L buttons 0x%x "
                    "trigger %.2f | R buttons 0x%x trigger %.2f\n",
            (unsigned long long)g_ctrl_reads,
            (unsigned long long)g_ctrl_reads_down,
            g_ctrl_last_buttons[0], (double)g_ctrl_last_trigger[0],
            g_ctrl_last_buttons[1], (double)g_ctrl_last_trigger[1]);
    g_ctrl_reads = g_ctrl_reads_down = 0;
}

static void fill_controller_state(int mask, void *out, int version) {
    memset(out, 0,
           version >= 6 ? 0x6c : version == 5 ? 0x64 : version == 4 ? 0x60 :
           version == 2 ? 0x40 : 0x30);
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
    g_ctrl_reads++;
    for (int h = 0; h < 2; h++) {
        g_ctrl_last_buttons[h] = in[h].buttons;
        g_ctrl_last_trigger[h] = in[h].index_trigger;
    }
    if (in[0].buttons || in[1].buttons ||
        in[0].index_trigger > 0.1f || in[1].index_trigger > 0.1f ||
        in[0].hand_trigger  > 0.1f || in[1].hand_trigger  > 0.1f)
        g_ctrl_reads_down++;
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
    // Thumbstick values, logged when non-zero (no flag) so a turning problem is
    // visible: the guest reads the RIGHT stick (f[10]=x, f[11]=y) for turning.
    // Throttled to one line per ~90 non-zero polls.
    if (f[8] != 0.0f || f[9] != 0.0f || f[10] != 0.0f || f[11] != 0.0f) {
        static unsigned tn;
        if ((tn++ % 90) == 0)
            fprintf(stderr, "  [ctrl] thumbstick L(%.2f,%.2f) R(%.2f,%.2f) "
                    "buttons=0x%x\n", (double)f[8], (double)f[9],
                    (double)f[10], (double)f[11], w[1]);
    }
    w[1] |= fake;                                       // Buttons
    w[2] |= fake;                                       // Touches
    if (fake_trig > 0.0f) {
        if (conn & OVRP_CTRL_LTOUCH) f[4] = fake_trig;  // LIndexTrigger
        if (conn & OVRP_CTRL_RTOUCH) f[5] = fake_trig;  // RIndexTrigger
    }
}

// KL_OVRP_VERBOSE: trace controller-state polls so a single run shows whether
// OVRInput polls per-frame or once, and exactly what ConnectedControllers we
// hand back for each mask. The connection bug hunt (Liminal falls to its GearVR
// gaze profile because it reads no hands connected) needs the frequency AND the
// value, and the guest's own log only shows first-seen.
static void ctrl_state_log(const char *name, int mask, void *out) {
    static int verbose = -1;
    if (verbose < 0) verbose = (int)kl_env_uint("KL_OVRP_VERBOSE", 0);
    if (!verbose) return;
    static unsigned total;
    unsigned n = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
    const uint32_t *w = (const uint32_t *)out;
    // First 24 calls in full, then one line every 600 so a per-frame poll shows
    // as a rising counter without flooding.
    if (n <= 24 || (n % 600) == 0)
        fprintf(stderr, "  [ctrl] #%u %s(mask=0x%x) -> Connected=0x%x Buttons=0x%x "
                "LIdxTrig=%.2f RIdxTrig=%.2f\n",
                n, name, (unsigned)mask, w[0], w[1],
                (double)((const float *)out)[4], (double)((const float *)out)[5]);
}

    // 64-byte ovrpControllerState2 by value via x8 (real plugin: stp q0..q3 of
// zeros on the failure path).
uint64_t klovrp_GetControllerState2_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState2");
    ovrp_log_arg("ovrp_GetControllerState2", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 2);
    ctrl_state_log("GetControllerState2", mask, out);
    return OVRP_SUCCESS;
}

// Same state, newest shape: (mask=w0, out=x1), 0x60 bytes (real plugin's
// memcpy size), plain ovrpResult. This is the one the game itself polls
// (OVRP_1_16_0::ovrp_GetControllerState4 via P/Invoke).
static uint64_t klovrp_GetControllerState4(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState4");
    ovrp_log_arg("ovrp_GetControllerState4", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 4);
    ctrl_state_log("GetControllerState4", mask, out);
    return OVRP_SUCCESS;
}


// ovrp_GetControllerState6 — the current OVRPlugin form (Red Matter 2). Same
// call shape as 5; fill_controller_state's version arg selects the struct size.
static uint64_t klovrp_GetControllerState6(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState6");
    ovrp_log_arg("ovrp_GetControllerState6", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 6);
    ctrl_state_log("GetControllerState6", mask, out);
    return OVRP_SUCCESS;
}

// Same state again, 0x64 bytes. GOLF+ is the first guest here to poll v5, and
// the layout is READ OUT OF THAT GUEST'S OWN libOVRPlugin.so rather than taken
// from a header. Three independent readings agree:
//
//  1. The copy-out at ovrp_GetControllerState5+0x44 (file 0x1ab974..0x1ab9a0)
//     is six q-stores to [x19]..[x19,#0x50] plus ONE `str w8, [x19, #0x60]`.
//     So the plugin writes exactly 0x64 bytes — v4's 0x60 plus a final u32.
//     For scale, ovrp_GetControllerState4 writes six q-stores and stops
//     (0x60), and ovrp_GetControllerState6 writes through [x19,#0x5c]+16
//     (0x6c). 0x60 / 0x64 / 0x6c, one step each.
//  2. The prefix is untouched: v5 takes the same (mask=w0, out=x1) pair, the
//     same plain-ovrpResult return (w0=0 ok, -1001 null out, -1002 no
//     service), and only the vtable slot differs (v4 [x8,#0x78], v5
//     [x8,#0x80], v6 [x8,#0x88]) — three shapes of one query, not three
//     queries.
//  3. The guest's own global-metadata.dat carries ControllerState4/5/6 and
//     the field names LThumbRestForce RThumbRestForce LStylusForce
//     RStylusForce L/RIndexTriggerCurl L/RIndexTriggerSlide (eight floats =
//     0x20) and, listed after them, L/RIndexTriggerForce (two more = the v6
//     delta). Eight floats laid over v4's 28 reserved bytes at +0x44 is
//     exactly 0x44 + 0x20 = 0x64, and ten is exactly 0x6c.
//
// So v5 is v4's prefix through +0x43 (ConnectedControllers/Buttons/Touches/
// NearTouches, the four triggers, the four vec2 sticks/touchpads, the two
// battery bytes and the two recenter counts) with v4's reserved tail replaced
// by the Touch Pro block:
//
//   +0x44 LThumbRestForce    +0x48 RThumbRestForce
//   +0x4C LStylusForce       +0x50 RStylusForce
//   +0x54 LIndexTriggerCurl  +0x58 RIndexTriggerCurl
//   +0x5C LIndexTriggerSlide +0x60 RIndexTriggerSlide
//
// Every one of those is a Touch PRO capability. A plain Touch controller —
// and the hand/pinch source this runtime actually synthesises from — reports
// them as zero on real hardware too, so leaving the block zeroed is the
// honest answer rather than a stub: the guest reads "no stylus, no thumb-rest
// force, no trigger curl", which is what a Quest 2 Touch reports. The part
// that would have been a silent lie is the SIZE, and that is measured.
//
// v6 (0x6c: v5 plus L/RIndexTriggerForce at +0x64/+0x68, both zero here) is
// registered below: Red Matter 2's plugin polls it, and a UE4.27
// OculusHMD polls ovrp_GetControllerState6 too. The two extra floats read
// zero.
static uint64_t klovrp_GetControllerState5(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState5");
    ovrp_log_arg("ovrp_GetControllerState5", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 5);
    return OVRP_SUCCESS;
}

// Two floats by out-pointer, measured off this APK's real libOVRPlugin
// (0x12a0c0: `stp s0, s1, [x19]`, ovrpResult, -1001 on NULL). The values are
// the plugin's own defaults, the same pair OVRPlugin's C# seeds
// NeckEyeDistance with; the SetUserNeckEyeDistance2 above is recorded state
// like every other setter here.
static uint64_t klovrp_GetUserNeckEyeDistance2(void *out) {
    ovrp_hit("ovrp_GetUserNeckEyeDistance2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    float *f = out;
    f[0] = 0.0805f; f[1] = 0.075f;
    return OVRP_SUCCESS;
}

// Deadpool VR's UOculusBodyTrackingComponent asks for the body skeleton once
// gameplay starts, even though the capability sweep already heard "no"
// (GetBodyTrackingSupported/Enabled in g_ovrp_bool_no). Measured off this
// guest (OculusMovement::GetBodyState, libUE4.so+0x945f71c): called as
// (w0=-1 step, w1=-1, x2=out buffer), and the out buffer is only read when
// the result is non-negative — the engine checks just the SIGN and maps a
// failure to TryGetBodyState=false, which puts the anim graph on its plain
// skeleton fallback. A negative result is the truth here: there is no body
// tracker on this platform. A zeroed struct would be a skeleton we invented.
//
// The parameters are DECLARED even though none is read, and that is the point:
// Pinball FX VR's libOVRPlugin (a newer build than Deadpool VR's) is the first one
// in this tree whose real ovrp_GetBodyState4 refuses a NULL x2 with -1001,
// which tools/ovrp_abi.py reads as "x2 is an out-parameter" — and a handler
// declared `(void)` cannot be checked against that at all, so the gate could
// only fail. Writing down the shape that was measured above lets it AGREE.
// The answer is unchanged and nothing is written through `out`; a negative
// result is precisely the contract that says the caller must not read it.
static uint64_t klovrp_GetBodyState4(int step, int flags, void *out) {
    (void)step; (void)flags; (void)out;
    ovrp_hit("ovrp_GetBodyState4");
    return (uint64_t)(int64_t)OVRP_FAIL_UNSUPPORTED;
}

// Same shape, 48 bytes (real plugin: three q-stores). v1 of the above; this
// is the one libunity's legacy VRDevice polls once a frame with mask=Touch.
uint64_t klovrp_GetControllerState_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState");
    ovrp_log_arg("ovrp_GetControllerState", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 1);
    ctrl_state_log("GetControllerState", mask, out);
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// Haptics: the seam running the OTHER way
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
//  ovrp_GetControllerHapticsDesc  -- how fast, how wide, how deep. Read ONCE,
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

// --- The playback model ----------------------------------------------------
//
// An envelope follower, not a pulse batcher: retirement IS playback. A sample
// that comes due is a sample the hand should be feeling, and klovrp_hap_drain
// accumulates what it retires into a level the frontend reads each frame.
// Nothing is buffered on this side and nothing is dropped, so the shape of a
// clip survives at whatever rate the frontend asks (90 Hz against a 320 Hz
// envelope, so ~3 samples a window).
//
// Batching whole pulses instead cannot work against this producer, measured on
// hardware: OVRHapticsOutput runs low-latency, keeping MinimumSafeSamplesQueued
// plus one frame's worth queued — 5 + 4 = 9 samples, 28 ms — which never
// reaches a 32 ms span, and holding samples back past their due time loses
// them outright (a ~100 ms note cut arrives as its tail, or as nothing).
//
// The 32 ms floor is ALVR's — an actuator cannot say anything in 10 ms — and
// applies to how long a level is HELD, not to how long emission waits. A 10 ms
// burst is reported at once and held for the rest of the window.
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
// transcribed from a header, because a shim entry is an unchecked contract and
// this one carries two pointers:
//
//  ovrp_GetControllerSampleRateHz  (+0x16fef0): w0 = controller, x1 = float*.
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
// ...2 form (real body +0x2f164) answers ovrpResult, so they are two wrappers
// rather than one table entry listed twice. The ARGUMENTS
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

// ...and the Touch Pro form UE5.3 reaches for:
//
//   ovrp_SetControllerLocalizedVibration(ovrpHapticsLocation locationMask,
//                                        float frequency, float amplitude,
//                                        ovrpController controllerMask)
//
// which the real wrapper (0x176d10) confirms register for register — it
// forwards w0 and w1 and leaves s0/s1 exactly where the caller put them, the
// same shape as ovrp_SetControllerVibration2 above.
//
// The LOCATION is dropped, deliberately, and it is a real drop rather than an
// omission: it selects between the hand, the thumb and the index actuators of a
// Touch Pro, and there is one vibration channel per hand here. Routing it to
// that channel is what a Touch (non-Pro) controller produces for the same call,
// and that is a controller this title certainly supports. Silence would not be
// — it would be a pinball game that never buzzes on a flipper.
static uint64_t klovrp_SetControllerLocalizedVibration(int location_mask,
                                                       float frequency,
                                                       float amplitude,
                                                       int controller_mask) {
    ovrp_hit("ovrp_SetControllerLocalizedVibration");
    (void)location_mask;
    klovrp_vibration(controller_mask, frequency, amplitude);
    return OVRP_SUCCESS;
}

// ---------------------------------------------------------------------------
// Environment Depth — the nine `ovrp_*EnvironmentDepth*` entry points.
//
// The headset's per-frame depth map of the ROOM, from the same cameras
// passthrough uses; UE5.3 asks for the frame descriptor during startup. Not
// here for the same reason passthrough is not, and answered the way the real
// plugin answers with no backend behind it (`cbz x8 -> mov w0, #-0x3ea`, e.g.
// 0x17c314): -1002.
//
// The two `*Supported` predicates are the exception and they answer
// FALSE-with-success, because the real ones (0x17c100, 0x17c150) write the
// answer through x0 and return 0. A caller that gates on them takes its own
// unsupported path and never asks for a texture at all, which is a better route
// through the guest than a failed getter it has to interpret.
static uint64_t klovrp_GetEnvironmentDepthSupported(int *out) {
    ovrp_hit("ovrp_GetEnvironmentDepthSupported");
    if (!out) return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
    *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetEnvironmentDepthHandRemovalSupported(int *out) {
    ovrp_hit("ovrp_GetEnvironmentDepthHandRemovalSupported");
    if (!out) return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
    *out = 0;
    return OVRP_SUCCESS;
}
#define KLOVRP_ENVDEPTH_ABSENT(fn, name)                                  \
    static uint64_t fn(void) {                                            \
        ovrp_hit(name);                                                   \
        return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;              \
    }
KLOVRP_ENVDEPTH_ABSENT(klovrp_InitializeEnvironmentDepth,
                       "ovrp_InitializeEnvironmentDepth")
KLOVRP_ENVDEPTH_ABSENT(klovrp_DestroyEnvironmentDepth,
                       "ovrp_DestroyEnvironmentDepth")
KLOVRP_ENVDEPTH_ABSENT(klovrp_StartEnvironmentDepth,
                       "ovrp_StartEnvironmentDepth")
KLOVRP_ENVDEPTH_ABSENT(klovrp_StopEnvironmentDepth,
                       "ovrp_StopEnvironmentDepth")
KLOVRP_ENVDEPTH_ABSENT(klovrp_SetEnvironmentDepthHandRemoval,
                       "ovrp_SetEnvironmentDepthHandRemoval")
KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthFrameDesc,
                       "ovrp_GetEnvironmentDepthFrameDesc")
KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTextureDesc,
                       "ovrp_GetEnvironmentDepthTextureDesc")
KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTexture,
                       "ovrp_GetEnvironmentDepthTexture")
KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTextureStageCount,
                       "ovrp_GetEnvironmentDepthTextureStageCount")
#undef KLOVRP_ENVDEPTH_ABSENT

// ovrpHapticsState is { int SamplesAvailable; int SamplesQueued; } — 8 bytes,
// so it comes home in x0 with SamplesAvailable in the low word. It must not be
// answered zero: that reads as "no room, nothing queued", and the managed side
// clamps what it sends to SamplesAvailable, so zero room sends nothing at all.
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
// Same questions, different ABI: where the un-suffixed pair return the struct
// (the desc through x8, the 8-byte state in x0), these take an OUT POINTER and
// return ovrpResult. Read out of RE4's
// own libOVRPlugin.so (+0x2f1a8 and +0x2f1e8): both `cbz` the pointer for
// -1001, take -1002 with no singleton, and otherwise TAIL-CALL the same backend
// method the un-suffixed form reaches — so the values are the same values, and
// answering them from anywhere but the shared body would be two descriptions of
// one controller.
//
// A struct-returning implementation under these names leaves the caller's
// buffer untouched and answers a success code — the same shape as an unwritten
// `ovrp_GetVersion2` out parameter: a read of a stack slot nobody wrote.
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
//    Result result = ovrp_GetAppHasInputFocus(out inputFocus);
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
    // What we ADVERTISE is not what the guest allocates. Unity multiplies the
    // recommended eye size by XRSettings.eyeTextureResolutionScale, so a guest
    // that changes that setting arrives here asking for a multiple of the size
    // kl_ovrp_set_eye_texture_size capped — and the cap, applied only to the
    // advertisement, does not bound this at all.
    //
    // SUPERHOT does exactly that when its menu opens: 2x on both axes, four
    // times the pixels, in one reconfigure. That took the footprint from 3936 MiB to 8196 MiB in under a second and jetsam killed
    // the process for `highwater` — with nothing in the log naming the ratio,
    // because both the advertisement and the request were only ever printed as
    // bare numbers hundreds of lines apart. So print the RATIO, at the moment
    // the guest asks, with the swapchain total it implies. Once per distinct
    // oversize size: the six calls of a reconfigure would otherwise say it six
    // times, and a guest that keeps the size says it forever.
    {
        int aw, ah;
        kl_ovrp_eye_texture_size(&aw, &ah);
        int req = w > h ? w : h, adv = aw > ah ? aw : ah;
        static int said_for;
        if (adv > 0 && req > adv && req != said_for) {
            said_for = req;
            double mib = (double)w * h * 8.0 * 2.0 * kl_ovrp_stage_count()
                       / (1024 * 1024);
            fprintf(stderr, "  [ovrp] the guest asked for %dx%d — %.2fx the "
                            "%dx%d we advertised, so %.0fx the pixels and "
                            "%.0f MiB of swapchain (before Unity's own targets, "
                            "which scale with it). KL_OVRP_EYE_MAX caps what we "
                            "ADVERTISE; it does not cap this\n",
                    w, h, (double)req / adv, aw, ah,
                    ((double)req / adv) * ((double)req / adv), mib);
        }
    }
    // The capture reads the frame back from the FBO this texture is attached
    // to — tell kl_glfb which names are eyes (it is a no-op consumer when the
    // null driver is doing the "rendering").
    kl_glfb_note_eye_texture(eye, stage, (uint32_t)handle);
    // When the host has MTLTextures for the compositor to sample, the eye
    // texture's storage IS one of them — glEGLImageTargetTexture2DOES in place
    // of glTexStorage2D, and nothing else about this function changes. The h,w
    // transposition below applies identically, so it is passed on in the same
    // order.
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
//    ovrp_CalculateEyeLayerDesc3(layout, ..., &desc)   "what shape is an eye layer?"
//     ovrp_SetupLayer(device, &desc, &layerId)          "make me one"
//     ovrp_CalculateEyeViewportRect / ...PreviewRect    "where does each eye sit in it?"
//     ovrp_CalculateLayerDesc + ovrp_SetupLayer         ...again for a 1x1 dummy layer
//   OculusDisplayProvider::CreateEyeTextures
//     ovrp_GetLayerTextureStageCount(layerId, &n)
//     ovrp_GetLayerTexture2(layerId, stage, eye, &color, &depth)
//
// So ovrp_GetLayerTexture2 is 1.40's ovrp_SetupEyeTexture2, and it routes
// through the same two seams: kl_glfb_note_eye_texture (the capture's eye-FBO
// census) and kl_glfb_bind_eye_mtl_texture (the MTLTexture backing). Nothing
// downstream of those has to know which Unity version asked.
//
// Every ABI here is read off the REAL libOVRPlugin.so in this APK rather than
// inferred from the caller: each function's argument shuffle and NULL check say
// exactly which register carries the out-param, and every one of them ends in
// `cmp w0,#0 / csel w0,w0,wzr,lt` — plain ovrpResult, success 0. That matters
// twice over here, because the family is a chain: a positive return from any of
// them reads as a failure and takes GfxThread_Start down with it (see
// g_ovrp_result_ok's note on ovrp_SetupDisplayObjects2).

// ovrpLayerDesc_EyeFov. The type NAME is the guest's own — libOculusXRPlugin
// exports `OculusSystem::SetRenderViewportScale(ovrpLayerDesc_EyeFov const&,
// ovrpEye, float, ovrpRecti*)` — and the OFFSETS are the ones the provider
// reads and writes in CreateLayer:
//
//   +0x08  read as an int and written back  -> TextureSize.w
//  +0x28 +0x2c                             -> Fov[0].LeftTan / .RightTan
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
#define KLOVRP_LAYOUT_STEREO     0
#define KLOVRP_LAYOUT_DOUBLEWIDE 2
#define KLOVRP_LAYOUT_ARRAY      3

// One entry per layer the provider sets up. Warmup guests (Beat Saber, RE4)
// make just an eye layer and a 1x1 dummy, so four slots were plenty. AC Nexus
// is the other shape: at the MENU it builds a whole set of OVROverlay UI panels
// (measured: ~11 concurrent quad sizes plus two Equirect layers), and with only
// four slots ovrp_SetupLayer failed from the third onward, returning
// OVRP_FAIL_INVALID_PARAM. The guest never got layer ids for its menu, retried
// SetupLayer every frame (thousands of times), and never reached a real
// ovrp_EndFrame4 - the render loop stalled on one submitted frame and the eye
// stayed frozen black. Real Quest OVRPlugin allows ~15; 32 gives headroom so
// the geometry-keyed reuse below does not thrash.
// (A UE lobby that keeps a restart pair of eye layers live and then asks
// for a second quad and passthrough needs eight; the table is larger.)
#define KLOVRP_MAX_LAYERS 64
static struct klovrp_layer {
    int      id;                                  // 0 = free; ids start at 1
    ovrp_layer_desc_eyefov desc;                  // as ovrp_SetupLayer received it
    uint32_t tex[KLOVRP_MAX_STAGES][2];           // GL names, per stage per eye
    int      is_eye;                              // eye layer, or the dummy
    int      used;                                // this slot has been set up before
    int      prev_id;                             // the id this slot answered to
                                                  // before its last destroy — what
                                                  // lets a Vulkan reuse find the
                                                  // old id's images (see SetupLayer)
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

// --- Loading-video skybox (AC Nexus) -------------------------------------
// AC Nexus paints its loading environment as a looping VIDEO on an OVROverlay
// Equirect layer (shape 5), fed by an Android MediaPlayer Surface that does
// not exist on visionOS - so the guest's own texture for it is the wild handle
// the kl_vulkan guard refuses, and the background renders as nothing. We cannot
// reproduce the Android decode, so the HOST plays the same mp4 itself
// (KleptonCompositor's LoadingVideoPlayer) onto the equirect the compositor
// already knows how to draw. The trigger is simply whether an Equirect layer is
// currently live: while the guest is loading (or stuck in shader warmup) the
// layer stays and the loading screen stays, and when the guest finishes it
// destroys the layer, g_loadvid_active clears, and the world shows through.
static int g_loadvid_active;
static int g_loadvid_suppressed;   // the guest asked to hide the loading screen
static void kl_loadvid_recount(void) {
    int any = 0;
    for (int i = 0; i < KLOVRP_MAX_LAYERS; i++)
        if (g_layers[i].id && !g_layers[i].is_eye && g_layers[i].desc.shape == 5) {
            any = 1; break;
        }
    if (any != g_loadvid_active)
        fprintf(stderr, "  [ovrp] loading-video skybox %s (an Equirect layer is %s)\n",
                any ? "ON" : "OFF", any ? "live" : "gone");
    g_loadvid_active = any;
}
// Swift-facing: 1 while the guest has a live Equirect loading layer.
int kl_loadingvideo_active(void) {
    // KL_LOADVID_OFF=1: force the loading skybox OFF regardless of whether the
    // Equirect loading layer is still live. AC Nexus never destroys that layer
    // once the menu is up, so the skybox stays painted over the (working) menu
    // overlays. This knob lets a run reveal the menu behind it without a rebuild;
    // the proper fix is to deactivate the skybox when the menu is being submitted.
    if (kl_env_on("KL_LOADVID_OFF", 0)) return 0;
    return g_loadvid_active;
}
// The guest's LoadingScreen.stopUpdatingLoadingScreen() - it wants the loading
// screen gone, so hide the skybox and reveal whatever the eye now holds (the
// menu, if it renders). A later show - a fresh Equirect SetupLayer - lifts this.
void kl_loadingvideo_stop(void) {
    // Deliberately a no-op for the skybox: AC Nexus calls this on every
    // loading-phase transition, not once at the end, so acting on it hid the
    // backdrop mid-load and uncovered the still-black eye. The skybox stays
    // tied to the Equirect layer's lifetime instead. (void)g_loadvid_suppressed.
    (void)g_loadvid_suppressed;
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
//    0x04 int          TextureStage
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

// ---------------------------------------------------------------------------
// ovrpLayerSubmit_EyeFov — the EYE layer's submit, and the one place the guest
// states the frustum it ACTUALLY rendered with
// ---------------------------------------------------------------------------
// An eye layer is never submitted as a bare ovrpLayerSubmit: the guest builds
// an ovrpLayerSubmitUnion (0x168 bytes) and passes a pointer to its base, which
// is why the real runtime is free to read past 0xbc for a layer whose shape is
// EyeFov. Read, like the base struct above, out of libOculusXRPlugin.so's own
// DWARF rather than inferred:
//
//     0x00  ovrpLayerSubmit        Base
//     0xbc  ovrpOctilinearLayout   OctilinearLayout[2]   (8 floats each)
//     0xfc  float                  DepthNear
//     0x100 float                  DepthFar
//     0x104 ovrpFovf               Fov[2]                <-- what this exists for
//     0x124 float                  MotionVectorDepthNear
//     0x128 float                  MotionVectorDepthFar
//     0x12c ovrpVector4f           MotionVectorScale
//     0x13c ovrpVector4f           MotionVectorOffset
//     0x14c ovrpPosef              AppSpaceDeltaPose
//                                                        (0x168 total)
//
// **Why this is not decoration: the guest does not have to render with the
// frustum we told it, and under Oculus SYMMETRIC PROJECTION it does not.** The
// long note above `klovrp_eye_tan` describes the transform; this is where the
// guest confesses it. Measured on GORN 2 (OVRPlugin 1.106, Vulkan, Multiview),
// straight out of `OculusDisplayProvider`:
//
//   GfxThread_PopulateNextFrameDesc  asks ovrp_GetNodeFrustum2 per eye, keeps
//                                    the pristine pair at +0x90/+0xa0, and —
//                                    when symmetric projection is on — collapses
//                                    its working pair at +0x70/+0x80 to the
//                                    per-component MAX of the two, the union.
//   SetupRenderParams                builds Unity's projection from that working
//                                    pair, i.e. BOTH eyes render the union cone.
//   CreateLayer                      widens the layer to
//                                      w = round(h*(L+R) / ((U+D)*96)) * 96
//                                    so the union's angular density survives —
//                                    for GORN 2, h=3234 with l=1.73205 r=1.00000
//                                    t=1.00000 b=1.19175 gives 53.25 -> 53 -> a
//                                    5088-wide layer from a 4032-wide desc,
//                                    which is exactly what the device reported.
//   SubmitFrame                      stores the working (union) pair here, in
//                                    Fov[2], and the FULL-WIDTH rects in
//                                    ViewportRect[2].
//
// So a run that reads only ViewportRect learns nothing: the rects are full width
// because the whole texture really is one eye's picture — drawn with a cone 26%
// wider than the one we handed out. Placing that picture with the per-eye cone
// squeezes each eye's image inward by that ratio, oppositely per eye, and the
// two views stop fusing. That is what "everything is doubled" looks like.
//
// The per-eye sub-rects DO exist in the provider (CreateLayer computes them at
// +0x50/+0x60 from the union and the pristine cones) but 1.106 submits them to
// nobody: they feed Unity's own render viewport, and only when the
// `optimizeMultiviewRenderRegions` setting is on. Reading Fov is therefore the
// statement that is always made, on every build, by every guest that submits an
// eye layer at all — which is why the fix lives here and not in a rect.
typedef struct {
    ovrp_layer_submit base;             // +0x00
    float     octilinear[2][8];         // +0xbc  ovrpOctilinearLayout[2]
    float     depth_near;               // +0xfc
    float     depth_far;                // +0x100
    ovrp_fovf fov[2];                   // +0x104
    float     mv_depth_near;            // +0x124
    float     mv_depth_far;             // +0x128
    float     mv_scale[4];              // +0x12c
    float     mv_offset[4];             // +0x13c
    float     app_space_delta_pose[7];  // +0x14c ovrpPosef
} ovrp_layer_submit_eyefov;             //  = 0x168

_Static_assert(offsetof(ovrp_layer_submit_eyefov, depth_near) == 0xfc, "eyefov");
_Static_assert(offsetof(ovrp_layer_submit_eyefov, fov) == 0x104, "eyefov");
_Static_assert(offsetof(ovrp_layer_submit_eyefov, mv_scale) == 0x12c, "eyefov");
_Static_assert(sizeof(ovrp_layer_submit_eyefov) == 0x168, "eyefov");

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
//  shape 1 (Cylinder) three words at +0xb0, +0xb4, +0xb8
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
// **The offset is not one number, because the header is not one size.** 0xb0 is
// where `ovrpLayerSubmit` ends in the plugin RE4 was built against, so that is
// where its Quad arm's Size sits. A NEWER plugin appends `HasBlendFactors`,
// `SrcBlendFactor` and `DstBlendFactor` to the same header — the three fields
// our own struct carries at 0xb0..0xbc, which came out of libOculusXRPlugin's
// DWARF — and its Quad arm therefore begins at 0xbc.
//
// Deadpool VR's UE4.27 is the second kind, and the failure was silent in exactly
// the way that costs a run: read at +0xb0 it yields `HasBlendFactors` and
// `SrcBlendFactor`, two zeroed ints, which as floats are 0.0 and 0.0. The
// splash quad then arrived with a world size of nothing, kl_overlay_build
// refused it for having no extent, and the compositor reported "no placement or
// no MTLTexture" for a layer whose texture was sitting right there.
//
// Probing both in order recovers RE4's answer unchanged — 0xb0 holds a real
// size there, so the first candidate wins — and Deadpool VR's as well. The
// plausibility window is what makes this a measurement rather than a guess: a
// pair of zeroed ints is not a size, and neither is a denormal or a kilometre.
// `sizeof(ovrpLayerSubmitUnion)` is 0x130, so both reads are in bounds either
// way.
#define KLOVRP_QUAD_SIZE_MIN 0.01f
#define KLOVRP_QUAD_SIZE_MAX 1000.0f
static const unsigned KLOVRP_QUAD_SIZE_OFFSETS[] = { 0xb0, 0xbc };
#define KLOVRP_QUAD_SIZE_NOFF (sizeof KLOVRP_QUAD_SIZE_OFFSETS / \
                               sizeof *KLOVRP_QUAD_SIZE_OFFSETS)

static int klovrp_quad_plausible(const float *q) {
    for (int i = 0; i < 2; i++)
        if (!(q[i] > KLOVRP_QUAD_SIZE_MIN) || !(q[i] < KLOVRP_QUAD_SIZE_MAX))
            return 0;
    return 1;
}

// The Quad arm's Size in metres, or 0 when neither candidate holds one.
// `*off_out` names the offset that answered, for the line that reports it.
static int klovrp_quad_size(const ovrp_layer_submit *s, float out[2],
                            unsigned *off_out) {
    for (unsigned i = 0; i < KLOVRP_QUAD_SIZE_NOFF; i++) {
        unsigned off = KLOVRP_QUAD_SIZE_OFFSETS[i];
        const float *q = (const float *)((const unsigned char *)s + off);
        if (!klovrp_quad_plausible(q)) continue;
        out[0] = q[0];
        out[1] = q[1];
        if (off_out) *off_out = off;
        return 1;
    }
    out[0] = out[1] = 0.0f;
    return 0;
}

static void klovrp_probe_submit_tail(const ovrp_layer_submit *s) {
    for (unsigned i = 0; i < KLOVRP_QUAD_SIZE_NOFF; i++) {
        unsigned off = KLOVRP_QUAD_SIZE_OFFSETS[i];
        const float *q = (const float *)((const unsigned char *)s + off);
        fprintf(stderr, "          union +%#x: %.4f %.4f  "
                        "(ovrpLayerSubmitQuad.Size, metres)%s\n",
                off, q[0], q[1],
                klovrp_quad_plausible(q) ? "  <-- plausible" : "");
    }
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

static int klovrp_quad_origin_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_QUAD_ORIGIN", 1);
    return on;
}

// **Where each quad went, and why.** One line per layer when it first appears
// and again whenever its PLACEMENT changes (origin, rebase, lock, or guest
// position by more than 2 cm) — never per frame. The line carries the guest's
// own pose, the origin and rebase in force, and the y the compositor will use
// in its floor-relative space, so "the logos were on the floor" and "the
// loading screen was too high" are answerable from the boot log alone.
// KL_OVRP_QUAD_TRAIL=0 silences; capped at 300 lines.
static void klovrp_quad_place_trail(const kl_ovrp_overlay *o, float guest_y,
                                    float add) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_QUAD_TRAIL", 1);
    if (!on) return;
    static struct { int id, live, origin, locked; float gx, gy, gz, add; }
        seen[KLOVRP_MAX_LAYERS * 4];
    static unsigned lines;
    int slot = -1;
    for (unsigned k = 0; k < sizeof seen / sizeof *seen; k++) {
        if (seen[k].live && seen[k].id == o->layer_id) { slot = (int)k; break; }
        if (!seen[k].live && slot < 0) slot = (int)k;
    }
    if (slot < 0) slot = (int)((unsigned)o->layer_id % (sizeof seen / sizeof *seen));
    int origin = g_origin_set ? g_tracking_origin : -1;
    int fresh = !seen[slot].live || seen[slot].id != o->layer_id;
    if (!fresh && seen[slot].origin == origin && seen[slot].locked == o->head_locked &&
        fabsf(seen[slot].add - add) < 0.02f &&
        fabsf(seen[slot].gx - o->pose[4]) < 0.02f &&
        fabsf(seen[slot].gy - guest_y) < 0.02f &&
        fabsf(seen[slot].gz - o->pose[6]) < 0.02f)
        return;
    seen[slot].live = 1; seen[slot].id = o->layer_id; seen[slot].origin = origin;
    seen[slot].locked = o->head_locked; seen[slot].add = add;
    seen[slot].gx = o->pose[4]; seen[slot].gy = guest_y; seen[slot].gz = o->pose[6];
    if (++lines > 300) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t t = ts.tv_sec;
    localtime_r(&t, &tm);
    fprintf(stderr, "  [ovrp] quad layer %d %02d:%02d:%02d.%03ld (%dx%d tex, %.2fx%.2f m, "
                    "flags %#x): guest pose (%.2f, %.2f, %.2f) %s, origin %s, rebase "
                    "%.3f m -> %s y %+.3f m%s (raw head %.3f m above the floor)%s\n",
            o->layer_id, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
            o->tex_w, o->tex_h, o->size[0], o->size[1], (unsigned)o->flags,
            o->pose[4], guest_y, o->pose[6],
            o->head_locked ? "HEAD-LOCKED" : "world-locked",
            klovrp_origin_name(origin), klovrp_origin_y(),
            o->head_locked ? "head-relative" : "placed at floor-space",
            guest_y + add,
            (!o->head_locked && !klovrp_quad_origin_on() && klovrp_origin_y() != 0.0f)
                ? " (KL_OVRP_QUAD_ORIGIN=0: rebase NOT added back)" : "",
            g_raw_head_have ? g_raw_head_y : 0.0f,
            lines == 300 ? " — quad trail capped at 300 lines" : "");
}

// KL_OVRP_QUAD=0 — the escape hatch. A composited overlay draws OVER the eye
// picture, so a layer placed wrongly is worse than one that is absent: this
// turns the whole non-eye pass off at the source, which is where it costs
// nothing and where the two compositors cannot disagree about it.
static int klovrp_quads_on(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_QUAD", 1);
        if (!on)
            fprintf(stderr, "  [ovrp] KL_OVRP_QUAD=0 — the guest's non-eye layers "
                            "are dropped rather than composited\n");
    }
    return on;
}

// ...and the fallback, for a Quad whose size neither candidate offset holds.
//
// A loading screen that is visible in the wrong place beats a black rectangle,
// and this is the only shape of guest that reaches here: a splash quad, alone
// in the submit list, while the eye picture is still black. So the fallback is
// scoped to exactly that — ONE non-eye layer, Quad, no readable size — and a
// guest submitting a whole UI's worth of overlays never takes it, because for
// that guest a wrongly placed panel really is worse than a missing one.
//
// Head-locked rather than world-locked on purpose. The pose is in the guest's
// tracking space and would be honoured if it meant anything, but a submit whose
// size we could not read is a submit whose layout we do not understand, and
// placing it at a pose from that same struct compounds one unknown with
// another. Centred in front of the viewer is a placement that cannot be wrong
// by more than its own angular size.
#define KLOVRP_QUAD_FALLBACK_M 1.20f   // the long edge, metres
#define KLOVRP_QUAD_FALLBACK_Z 1.50f   // distance in front of the head, metres

static void klovrp_quad_fallback(kl_ovrp_overlay *o) {
    // Read once: this runs per submit while the splash is up, and env reads
    // stay off the render path.
    static int read;
    static float longest_env, z_env;
    if (!read) {
        longest_env = kl_env_float("KL_OVRP_QUAD_FALLBACK_M", KLOVRP_QUAD_FALLBACK_M);
        z_env       = kl_env_float("KL_OVRP_QUAD_FALLBACK_Z", KLOVRP_QUAD_FALLBACK_Z);
        read = 1;
    }
    float longest = longest_env, z = z_env;
    if (!(longest > 0.0f)) return;                 // =0 disables the fallback
    if (!(z > 0.05f)) z = KLOVRP_QUAD_FALLBACK_Z;
    // The texture's aspect, so the picture is not stretched by the placement.
    float w = longest, h = longest;
    if (o->tex_w > 0 && o->tex_h > 0) {
        if (o->tex_w >= o->tex_h) h = longest * (float)o->tex_h / (float)o->tex_w;
        else                      w = longest * (float)o->tex_w / (float)o->tex_h;
    }
    o->size[0] = w;
    o->size[1] = h;
    o->head_locked = 1;
    // Identity orientation facing the viewer, z metres along the head's own
    // -Z. kl_overlay_build multiplies this by origin_from_device for a
    // head-locked layer, so it is stated in device space here.
    o->pose[0] = o->pose[1] = o->pose[2] = 0.0f;
    o->pose[3] = 1.0f;
    o->pose[4] = o->pose[5] = 0.0f;
    o->pose[6] = -z;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ovrp] layer %d is a Quad whose world size is at neither "
                        "known offset — compositing it HEAD-LOCKED at %.2fx%.2f m, "
                        "%.2f m ahead, rather than dropping it "
                        "(KL_OVRP_QUAD_FALLBACK_M=0 to drop it instead)\n",
                o->layer_id, w, h, z);
    }
}

static void klovrp_record_overlays(const void *layer_submits, int count) {
    const ovrp_layer_submit *const *list = layer_submits;
    kl_ovrp_overlay tmp[KLOVRP_MAX_LAYERS];
    int n = 0;
    if (!klovrp_quads_on()) list = NULL;
    for (int i = 0; list && i < count && n < KLOVRP_MAX_LAYERS; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        // Raw submit dump (KL_OVRP_SUBMIT_DUMP, default on): overlays land at
        // impossible positions on some guests (wrath2: 0,-9.5,-25) while others
        // read fine (olar: 0,1.48,-0.9) from the SAME +0x28 pose / +0xbc size
        // offsets — so those guests' ovrpLayerSubmit has a different field layout.
        // Print the first 0xd0 bytes of a NON-eye submit once per layer as words
        // + floats so the real pose(quat+xyz) and Quad size offsets are readable.
        {
            static int dumped, on = -1;
            if (on < 0) on = kl_env_on("KL_OVRP_SUBMIT_DUMP", 1);
            struct klovrp_layer *dl = klovrp_layer(s->layer_id);
            if (on && dumped < 6 && dl && !dl->is_eye) {
                dumped++;
                const unsigned char *b = (const unsigned char *)s;
                fprintf(stderr, "  [ovrp] SUBMIT DUMP layer %d (%d bytes shown):\n",
                        s->layer_id, 0xd0);
                for (int off = 0; off < 0xd0; off += 16) {
                    unsigned int w[4]; float f[4];
                    memcpy(w, b + off, sizeof w); memcpy(f, b + off, sizeof f);
                    fprintf(stderr, "    +0x%02x: %08x %08x %08x %08x | "
                            "%.4g %.4g %.4g %.4g\n", off,
                            w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
                }
            }
        }
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        int effective_id = s->layer_id;
        if (!l) {
            // A submit can outlive its layer. UE4 destroys the toast layer and
            // sets up its replacement on the game thread while the RHI thread
            // goes on submitting the OLD id for a few more frames — wrath2's
            // "compiling shaders" toast does this once a second, and dropping
            // those submits blinked the toast off for exactly those frames.
            // The slot remembers the id it answered to (prev_id, kept for the
            // Vulkan rekey), so the stale submit is filed against the slot's
            // storage instead of the floor. Which id reaches the compositor
            // follows the rekey: once the slot is set up again its images
            // answer to the NEW id; between destroy and setup they still
            // answer to the old one.
            for (int k = 0; k < KLOVRP_MAX_LAYERS; k++)
                if (g_layers[k].used && g_layers[k].prev_id == s->layer_id) {
                    l = &g_layers[k];
                    if (l->id) effective_id = l->id;
                    break;
                }
            if (l) {
                static unsigned bridged;
                if (bridged++ % 128 == 0)
                    fprintf(stderr, "  [ovrp] overlay submit for destroyed layer %d "
                                    "bridged to its slot (now layer %d, %u so far)\n",
                            s->layer_id, l->id, bridged);
            }
        }
        if (!l || l->is_eye) continue;
        kl_ovrp_overlay *o = &tmp[n++];
        memset(o, 0, sizeof *o);
        o->layer_id = effective_id;
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
        // The other direction of klovrp_origin_y. This pose came FROM the guest,
        // so it is in the guest's tracking space; the compositor places it in
        // its own, floor-relative one. Under a rebased EyeLevel the guest's y=0
        // is the eye, so a world-locked quad at y=0 ("straight ahead") has to go
        // UP by the rebase, or it lands on the floor — FNAF: Help Wanted 2's
        // opening logos (a 1920x1080 stereo-layer quad at 0,0,-1.7, flags 0)
        // did exactly that with only the head half of the rebase in place. A
        // head-locked layer is relative to the head and has no origin in it:
        // left alone.
        // KL_OVRP_QUAD_ORIGIN=0 reverts (quads placed at the guest's raw y).
        float guest_y = o->pose[5], add = 0.0f;
        if (!o->head_locked && klovrp_quad_origin_on()) add = klovrp_origin_y();
        o->pose[5] += add;
        // An OVRPlugin layer's storage is the guest's own, and which way up it
        // is follows from the API it rendered with — the same question, and the
        // same answer, as the eye textures beside it. Only the Vulkan path
        // backs a non-eye layer with anything a compositor can sample today
        // (kl_vulkan_layer_mtl_texture), and Vulkan's origin is the top left.
        o->origin_top_left = kl_vulkan_guest_active();
        // The quad's world size, out of the union's own arm — see klovrp_quad_size,
        // which is where the offsets and the reason there are two of them live.
        if (o->shape == 0) {
            unsigned off = 0;
            if (klovrp_quad_size(s, o->size, &off)) {
                // Once per run: WHICH header this guest's plugin was built with
                // is the one fact that turns a black splash into a visible one,
                // and it is not recoverable from any other line.
                static unsigned said;
                if (said != off) {
                    said = off;
                    fprintf(stderr, "  [ovrp] the Quad arm's Size is at +%#x on this "
                                    "guest's ovrpLayerSubmitUnion (%.2fx%.2f m)\n",
                            off, o->size[0], o->size[1]);
                }
            }
        }
        // Back a GLES non-eye layer with something a compositor can sample.
        // The Vulkan half is already handled — o is read back through
        // kl_vulkan_layer_mtl_texture in encodeOverlays — but on GLES nothing
        // here ever gave the quad's guest texture MTLTexture storage, so
        // kl_glfb_layer_mtl_texture answered NULL and the panel was dropped
        // ("no MTLTexture — not composited"): twd2's entire menu. COPY the
        // guest's presented texture into storage of ours (kl_glfb_mirror_layer),
        // NOT the eye path's re-point — re-pointing a live-attached quad texture
        // broke a guest's rendering outright (see kl_glfb.h). This runs on the
        // guest's own RHI/submit thread, so its GL context is current for the
        // blit, exactly as the OpenXR path's klxr_back_layer_images relies on.
        if (!kl_vulkan_guest_active() && kl_glfb_has_mtl_layer_provider() &&
            (unsigned)o->stage < KLOVRP_MAX_STAGES) {
            uint32_t gtex = l->tex[o->stage][0];
            if (gtex) {
                const char *fn = NULL;
                uint32_t gf = klovrp_gl_format(l->desc.format, &fn);
                if (!gf) gf = KL_OVRP_TEXFMT_EYE;
                kl_glfb_mirror_layer(effective_id, o->stage, gtex, -1,
                                     l->desc.texture_size.w,
                                     l->desc.texture_size.h, gf);
            }
        }
        klovrp_quad_place_trail(o, guest_y, add);
    }
    // The fallback, scoped to the lone splash quad — see klovrp_quad_fallback.
    if (n == 1 && tmp[0].shape == 0 &&
        !(tmp[0].size[0] > 0.0f && tmp[0].size[1] > 0.0f))
        klovrp_quad_fallback(&tmp[0]);
    // Overlay PERSISTENCE. The guest submits its overlay layers intermittently:
    // wrath2's "compiling shaders" toast is destroyed+recreated about once a
    // second and only ~1 frame in 4 carries it (EndFrame4 alternates layers=1 /
    // layers=2), and olar's menu does the same. Replacing the set wholesale each
    // frame therefore blinks the panel off on every gap — the "flashing". So when
    // a frame carries FEWER overlays than the set we are already showing, keep the
    // fuller set for a short TTL instead of dropping to the smaller one; a
    // transient drop (recreation, half-rate submit) then never reaches the screen,
    // while a real removal still clears once the TTL lapses. The bridging above
    // handles a submit that NAMES a destroyed layer; this handles a frame that
    // omits it entirely. KL_OVRP_OVERLAY_PERSIST=0 restores strict per-frame.
    static int persist = -1;
    if (persist < 0) persist = kl_env_on("KL_OVRP_OVERLAY_PERSIST", 1);
    enum { KLOVRP_OVERLAY_TTL = 12 };   // frames a dropped overlay is held for
    pthread_mutex_lock(&g_frames.mu);
    static unsigned rec_frame, last_take;
    rec_frame++;
    if (persist && n < g_overlays.n && (rec_frame - last_take) < KLOVRP_OVERLAY_TTL) {
        // Keep the fuller set we are already showing — this frame's drop is
        // transient. g_overlays is left untouched.
    } else {
        memcpy(g_overlays.v, tmp, sizeof tmp);
        g_overlays.n = n;
        last_take = rec_frame;
    }
    pthread_mutex_unlock(&g_frames.mu);
}

// ...and the same list, filed by a guest that never speaks OVRPlugin. See
// kl_ovrp.h. One storage, one lock, one reader: a second list filled by a
// second producer is how the two answers drift, and no process runs both.
void kl_ovrp_overlays_external(const kl_ovrp_overlay *v, int n) {
    if (n < 0) n = 0;
    if (n > KLOVRP_MAX_LAYERS) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] %d non-eye layers submitted and only %d fit "
                            "the record — the rest are NOT composited\n",
                    n, KLOVRP_MAX_LAYERS);
        }
        n = KLOVRP_MAX_LAYERS;
    }
    // KL_OVERLAY_PERSIST: while the guest cycles its loading screen it submits
    // frames of only Equirect (shape 5) layers, which the compositor skips -
    // leaving the view black between the brief menu frames (flash-then-black).
    // Keep the last set that HAD a compositable Quad instead of replacing it with
    // an all-equirect or empty one, so the menu stays up. Off by default so no
    // other target regresses.
    static int persist = -1;
    if (persist < 0) persist = kl_env_on("KL_OVERLAY_PERSIST", 0);
    if (persist) {
        int quads = 0;
        // Non-head-locked, because layer 3 is a head-locked full-view cover the
        // guest keeps submitting during loading; the compositor skips it under
        // KL_OVERLAY_SKIP_HEADLOCKED, so counting it as "has content" would let
        // an all-cover frame blank the real world-locked menu behind it.
        for (int i = 0; i < n; i++) if (v && v[i].shape == 0 && !v[i].head_locked) { quads = 1; break; }
        if (!quads) return;   // keep whatever is already published
    }
    pthread_mutex_lock(&g_frames.mu);
    if (n && v) memcpy(g_overlays.v, v, (size_t)n * sizeof *v);
    g_overlays.n = n;
    pthread_mutex_unlock(&g_frames.mu);
}

// The projection layers of the current frame — see kl_ovrp.h. One storage, one
// lock, one reader, exactly as the overlay list beside it; nothing but an
// OpenXR guest ever files it, and a count of zero means "composite the eye the
// way you always did".
static struct {
    kl_ovrp_proj_layer v[KLOVRP_MAX_LAYERS];
    int                n;
} g_proj_layers;

void kl_ovrp_proj_layers_external(const kl_ovrp_proj_layer *v, int n) {
    if (n < 0) n = 0;
    if (n > KLOVRP_MAX_LAYERS) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] %d projection layers submitted and only %d "
                            "fit the record — the rest are NOT composited\n",
                    n, KLOVRP_MAX_LAYERS);
        }
        n = KLOVRP_MAX_LAYERS;
    }
    pthread_mutex_lock(&g_frames.mu);
    if (n && v) memcpy(g_proj_layers.v, v, (size_t)n * sizeof *v);
    g_proj_layers.n = n;
    pthread_mutex_unlock(&g_frames.mu);
}

int kl_ovrp_proj_layer_count(void) {
    pthread_mutex_lock(&g_frames.mu);
    int n = g_proj_layers.n;
    pthread_mutex_unlock(&g_frames.mu);
    return n;
}

// The whole list under ONE lock — see kl_ovrp.h. Reading it an entry at a time
// lets the guest replace it in between, which draws half of one frame's layers
// with half of the next's.
int kl_ovrp_proj_layers_snapshot(kl_ovrp_proj_layer *out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int n = g_proj_layers.n;
    if (n > max) n = max;
    if (n > 0) memcpy(out, g_proj_layers.v, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_frames.mu);
    return n;
}

// See kl_ovrp.h. One int, no lock: it is written by the guest's frame thread and
// read by the compositor's, an aligned int is not torn on any platform this
// runs on, and a compositor that reads the previous frame's answer draws the
// previous frame's layer set — which is what it does with the overlay list
// beside it anyway.
static int g_eye_layer_live = 1;
void kl_ovrp_eye_layer_external(int submitted) {
    if (g_eye_layer_live == !!submitted) return;
    g_eye_layer_live = !!submitted;
    // Both directions, because both are scene transitions and this is the line
    // that says which one the compositor is in.
    fprintf(stderr, "  [ovrp] the guest's frame %s an eye picture — the eye pass "
                    "is %s\n",
            g_eye_layer_live ? "now carries" : "no longer carries",
            g_eye_layer_live ? "on" : "off, leaving only its composition layers");
}
int kl_ovrp_eye_layer_live(void) { return g_eye_layer_live; }

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

// The last eye-layer viewport any frame submitted, and how many frames named no
// eye layer at all. See the carry-forward below; both are read by the census.
static int      g_vp_have_last;
static int      g_vp_last[8];
static int      g_vp_last_of[2];
static uint64_t g_vp_no_eye_layer;      // frames whose submit named no eye layer
static uint64_t g_vp_carried;           // ...of those, how many were carried

uint64_t kl_ovrp_viewport_carry_stats(uint64_t *carried) {
    if (carried) *carried = g_vp_carried;
    return g_vp_no_eye_layer;
}

static int klovrp_submit_viewports(const void *layer_submits, int count, int vp[8],
                                   int of[2]) {
    const ovrp_layer_submit *const *list = layer_submits;
    for (int i = 0; list && i < count; i++) {
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
        // **KL_OVRP_ARRAY_EYE_VIEWPORT: the right eye's rect, when the eyes are
        // slices of ONE image.** Off by default; a target row turns it on
        // (Pinball FX VR), where the rect measured on device is not a rect
        // anything could have rendered into:
        //
        //     eye0 0,0 4863x3914   eye1 1302,0 4865x3914   of 6628x4208
        //
        // Under ovrpLayout_Array the two eyes are two LAYERS of a single
        // VkImage and the guest reaches them with multiview, which rasterizes
        // every view of a subpass through the SAME viewport — so a right eye
        // whose origin is 1302 px along, INSIDE the left eye's own rect, cannot
        // describe where the right eye was drawn. What it does describe, once
        // the compositor crops by it, is the right eye's picture slid sideways
        // against the left eye's: two images the eyes cannot fuse, which is
        // exactly what was reported from the headset — "one image on the left,
        // one next to it on the right, both visible simultaneously".
        //
        // The correction is the narrow one and it invents nothing: use the LEFT
        // eye's rect for both, i.e. the rect the shared render pass actually
        // had. It is gated rather than defaulted because differing rects ARE
        // legitimate elsewhere — BONELAB is the guest the compositor's per-eye
        // unwarp block was built for (KleptonCompositor.swift, unwarpGrid) —
        // and this is a claim about an ARRAY layer on one title, not about the
        // per-eye path in general. KL_OVRP_ARRAY_EYE_VIEWPORT=0 restores the
        // submitted rects as the A/B.
        if (l->desc.layout == KLOVRP_LAYOUT_ARRAY &&
            kl_env_on("KL_OVRP_ARRAY_EYE_VIEWPORT", 0) &&
            memcmp(vp, vp + 4, 4 * sizeof *vp) != 0) {
            static int said_shared;
            if (!said_shared++)
                fprintf(stderr,
                        "  [ovrp] the eyes are slices of ONE Array image but "
                        "submitted DIFFERENT rects (%d,%d %dx%d vs %d,%d %dx%d) "
                        "— multiview rasterizes both slices through one "
                        "viewport, so eye0's rect is used for both "
                        "(KL_OVRP_ARRAY_EYE_VIEWPORT=0 restores the submitted "
                        "pair)\n",
                        vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7]);
            memcpy(vp + 4, vp, 4 * sizeof *vp);
        }
        // Said when it CHANGES, which is the event, and said for both eyes
        // because a per-eye difference is the one thing a single unwarp grid
        // cannot express (see the compositors). Silent on a run that never
        // scales, so this costs a healthy title nothing.
        if (!g_vp_have_last || memcmp(g_vp_last, vp, sizeof g_vp_last) != 0) {
            g_vp_have_last = 1;
            memcpy(g_vp_last, vp, sizeof g_vp_last);
            fprintf(stderr, "  [ovrp] render viewport: eye0 %d,%d %dx%d  eye1 %d,%d %dx%d "
                            "of a %dx%d eye texture (%.1f%% of the width)\n",
                    vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7],
                    l->desc.texture_size.w, l->desc.texture_size.h,
                    l->desc.texture_size.w > 0
                        ? 100.0 * (double)vp[2] / (double)l->desc.texture_size.w : 0.0);
        }
        g_vp_last_of[0] = of[0];
        g_vp_last_of[1] = of[1];
        return 1;
    }
    // ---- no eye layer in this frame's submit ------------------------------
    //
    // **This is what put Into the Radius's whole picture on the display TWICE.** An Unreal
    // guest does not submit its eye layer on every frame: while it is compiling
    // shaders, loading a level, or showing a splash it submits only its quad
    // layers. The record for such a frame was filed with the viewport left at
    // its BeginFrame memset — all zeroes, which every reader is entitled to
    // read as "the whole texture".
    //
    // For a Stereo or Array guest that is merely inaccurate: one texture per
    // eye, so "the whole texture" is the right rect by accident. For a
    // **DoubleWide** guest it is catastrophic and silent. Both eyes are served
    // ONE texture carrying both views side by side (kl_glfb.h), and the crop is
    // the only thing separating them: with it refused, each eye samples the
    // entire canvas and sees BOTH halves — the picture, twice, side by side,
    // in both eyes. Every call on the path succeeds, the eye texture is
    // perfect, and the host — which reads that texture back directly and never
    // composites — reports a flawless run. Measured by the composite audit in
    // kl_reproject.h, which is the instrument that finally caught it.
    //
    // So: CARRY the last eye-layer viewport forward. That is not an invention.
    // The eye TEXTURE a compositor samples on such a frame is itself the last
    // one the guest drew — the eye pass shows stale eye storage by design when
    // the guest submits no new picture — so the rect it was drawn with is the
    // only rect that PAIRS with it. Zero pairs with nothing; it is a third
    // answer that describes neither the old picture nor a new one.
    //
    // Nothing is carried before the first eye-layer submit has been seen: with
    // no measurement there is nothing honest to carry, and the whole-texture
    // fallback is what every guest that cannot answer has always had.
    // KL_OVRP_VIEWPORT_CARRY=0 restores that behaviour outright, so the A/B is
    // one variable.
    g_vp_no_eye_layer++;
    static int carry = -1;
    if (carry < 0) carry = kl_env_on("KL_OVRP_VIEWPORT_CARRY", 1);
    if (!carry || !g_vp_have_last) return 0;
    memcpy(vp, g_vp_last, sizeof g_vp_last);
    of[0] = g_vp_last_of[0];
    of[1] = g_vp_last_of[1];
    g_vp_carried++;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ovrp] a frame's submit named NO eye layer — carrying the "
                        "last render viewport (eye0 %d,%d %dx%d  eye1 %d,%d %dx%d of "
                        "%dx%d) into its record rather than leaving it zero, which "
                        "every compositor reads as 'the whole texture' and which on a "
                        "DoubleWide eye shows BOTH halves to BOTH eyes "
                        "(KL_OVRP_VIEWPORT_CARRY=0 to go back)\n",
                vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7],
                of[0], of[1]);
    }
    return 1;
}

// KL_OVRP_SUBMIT_FOV=0 — go back to recording the frustum we HANDED OUT rather
// than the one the guest says it rendered with.
//
// It defaults ON because the submitted pair is the guest's own statement and
// ours is only an offer, and because the two are identical for every guest that
// simply honours what it was told — which is every host run and every title
// measured before GORN 2. It exists as an A/B for the case this cannot see
// coming: a plugin build that fills Fov[] with something other than what it
// rendered. Turning it off restores the previous behaviour exactly.
static int klovrp_use_submit_fov(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_SUBMIT_FOV", 1);
        if (!on)
            fprintf(stderr, "  [ovrp] KL_OVRP_SUBMIT_FOV=0 — the frame record keeps "
                            "the frustum we offered, not the one the guest submits\n");
    }
    return on;
}

// The eye layer's submitted Fov pair, as eight tangents in the frame record's
// own {left, right, top, bottom} order (ovrpFovf is {Up, Down, Left, Right} —
// the transposition happens here, at the one place that speaks the guest's ABI,
// exactly as klovrp_fill_eye_desc does it in the other direction).
//
// **Fov and ViewportRect are ONE statement and are read as one.** Fov is the
// cone the guest projected across the whole layer texture; ViewportRect is the
// part of that texture this eye's picture is in. Taking either alone is a wrong
// answer on some build that exists:
//
//   rect = full texture   (GORN 2, OVRPlugin 1.106)   the cone IS the Fov, the
//                                                     union, and the crop is a
//                                                     no-op.
//   rect = a sub-rect     (BONELAB's build, measured  the Fov is still the
//                          2880x2400 with eye 1 at    union, but the picture is
//                          609,0 2271x2400)           only the eye's own cone
//                                                     inside it.
//
// Projection is linear in tangent space across the texture, so narrowing the
// cone to the rect is the same arithmetic either way:
//
//   left'  = Fov.left  - (x / W) * (Fov.left + Fov.right)
//   right' = ((x + w) / W) * (Fov.left + Fov.right) - Fov.left
//
// and run against BONELAB's measured numbers that returns l=1.000 r=1.732 for
// eye 1 — the eye's own cone, recovered exactly. For GORN 2 it returns the
// union unchanged. One rule, both builds, and it is the identity for every
// guest that renders its own cone into its whole texture.
//
// Returns 0 and leaves `tan` alone unless every one of the eight results is a
// finite, positive, sane tangent. A guest that leaves the field zeroed — which
// the ABI permits and which is indistinguishable from "not filled in" — must
// not be able to collapse the picture to a degenerate cone; the previous
// behaviour (the frustum we handed out, already in the record from BeginFrame)
// is the right answer there and stays.
//
// `vp`/`of` are klovrp_submit_viewports' answer, or NULL when there was none to
// read, in which case the Fov is taken as covering the whole texture.
static int klovrp_submit_eye_fov(const void *layer_submits, int count,
                                 const int *vp, const int *of, float tan[8]) {
    const ovrp_layer_submit *const *list = layer_submits;
    if (!list || count <= 0 || !klovrp_use_submit_fov()) return 0;
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l || !l->is_eye) continue;
        // Only now, and only for the EYE layer: an overlay submit is a Quad or
        // Cylinder union member and has no Fov at this offset at all.
        const ovrp_layer_submit_eyefov *e = (const ovrp_layer_submit_eyefov *)s;
        float t[8];
        for (int eye = 0; eye < 2; eye++) {
            t[eye * 4 + 0] = e->fov[eye].left;
            t[eye * 4 + 1] = e->fov[eye].right;
            t[eye * 4 + 2] = e->fov[eye].up;
            t[eye * 4 + 3] = e->fov[eye].down;
        }
        for (int k = 0; k < 8; k++)
            if (!(t[k] > 0.01f) || !(t[k] < 100.0f)) return 0;
        // Narrowed to the rect, per eye. Skipped entirely — not merely a no-op —
        // when the rect is the whole texture, so a run with no rect to read and
        // a run whose rect covers everything take the same path.
        if (vp && of && of[0] > 0 && of[1] > 0) {
            for (int eye = 0; eye < 2; eye++) {
                const int *r = vp + eye * 4;
                if (r[2] <= 0 || r[3] <= 0) continue;
                if (r[0] == 0 && r[1] == 0 && r[2] == of[0] && r[3] == of[1]) continue;
                float *q = t + eye * 4;
                float h = q[0] + q[1], v = q[2] + q[3];
                float l0 = q[0] - (float)r[0] / (float)of[0] * h;
                float r0 = (float)(r[0] + r[2]) / (float)of[0] * h - q[0];
                float t0 = q[2] - (float)r[1] / (float)of[1] * v;
                float b0 = (float)(r[1] + r[3]) / (float)of[1] * v - q[2];
                if (l0 > 0.01f && r0 > 0.01f && t0 > 0.01f && b0 > 0.01f &&
                    l0 < 100.0f && r0 < 100.0f && t0 < 100.0f && b0 < 100.0f) {
                    q[0] = l0; q[1] = r0; q[2] = t0; q[3] = b0;
                }
            }
        }
        memcpy(tan, t, sizeof t);
        // Said when the guest's answer stops matching ours, which is the event:
        // a title that renders what it was told costs this nothing, and a title
        // that does not has just changed where its picture belongs.
        static int said;
        static float last[8];
        int changed = !said;
        for (int k = 0; k < 8 && !changed; k++) changed = last[k] != t[k];
        if (changed) {
            said = 1;
            memcpy(last, t, sizeof last);
            const float *o0 = klovrp_eye_tan(0), *o1 = klovrp_eye_tan(1);
            int same = o0[0] == t[0] && o0[1] == t[1] && o0[2] == t[2] && o0[3] == t[3] &&
                       o1[0] == t[4] && o1[1] == t[5] && o1[2] == t[6] && o1[3] == t[7];
            if (!same)
                fprintf(stderr, "  [ovrp] the guest RENDERED a different frustum than it "
                                "was offered: rendered eye0 l=%.5f r=%.5f t=%.5f b=%.5f, "
                                "eye1 l=%.5f r=%.5f t=%.5f b=%.5f (offered eye0 l=%.5f "
                                "r=%.5f, eye1 l=%.5f r=%.5f) — the frame record takes the "
                                "submitted pair, so the composite places what was drawn\n",
                        (double)t[0], (double)t[1], (double)t[2], (double)t[3],
                        (double)t[4], (double)t[5], (double)t[6], (double)t[7],
                        (double)o0[0], (double)o0[1], (double)o1[0], (double)o1[1]);
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
// ---------------------------------------------------------------------------
// What layout an eye layer actually GETS.
//
// ovrp_CalculateEyeLayerDesc* is not an echo service: its whole job is "given
// these parameters, what does an eye layer on THIS runtime look like?", and the
// answer is the plugin's to give. The guest passes a layout it would like; the
// desc that comes back is what it will be handed, and UE4's OculusHMD reads
// `Desc.Layout` out of it to decide the shape of the render target it allocates
// (one array texture with a slice per eye, or one texture per eye).
//
// Tetris Effect asks for ovrpLayout_Array unconditionally — the Oculus mobile
// plugin's Android default — even though nothing here advertises
// GL_OVR_multiview2 and kl_ovrp_multiview() answers no. Honouring that on the GL
// gateway is not possible (klovrp_GetLayerTexture2 says why: two 2D names bound
// to what the guest believes is one two-slice array renders to the wrong storage
// with no error anywhere), and REFUSING it is worse than it sounds: UE4 takes the
// failed GetLayerTexture2, leaves EyeLayer_RenderThread's swap chain null, and
// faults on the render thread several calls later (measured: SIGSEGV in
// _platform_memmove with a NULL destination, from libUE4+0x9ef9118, right after
// the refusal).
//
// So the layer is DESCRIBED as Stereo, which is the layout this runtime can
// really provide, and the guest allocates for that. It is the plugin answering
// truthfully about its own capability rather than a behaviour hack: the two
// answers that must agree — this and kl_ovrp_multiview() — now do.
//
// The Vulkan path is untouched: there Array is genuinely served (a two-layer
// VkImage, both eyes the same handle), so an Array request there is honoured.
//
// `KL_OVRP_EYE_LAYOUT=<n>` forces a specific ovrpLayout and is the A/B — set it
// to 3 to put the Array request back exactly as the guest made it.
static int klovrp_eye_layout(int asked) {
    const char *forced = kl_env_str("KL_OVRP_EYE_LAYOUT", NULL);
    if (forced && *forced) {
        int v = atoi(forced);
        static int said;
        if (!said++)
            fprintf(stderr, "  [ovrp] KL_OVRP_EYE_LAYOUT=%d: eye layers are "
                            "described as layout %d whatever the guest asks "
                            "(it asked %d)\n", v, v, asked);
        return v;
    }
    if (asked != KLOVRP_LAYOUT_ARRAY)
        return asked;
    if (asked == KLOVRP_LAYOUT_ARRAY && kl_vulkan_guest_active() && kl_ovrp_multiview())
        return asked;
    static int said;
    if (!said++)
        fprintf(stderr, "  [ovrp] eye layer asked for layout %d (Array); this "
                        "gateway does not serve it, so the layer is described as "
                        "layout %d (Stereo, one texture per eye) — the layout it "
                        "can actually be handed. KL_OVRP_EYE_LAYOUT=%d restores "
                        "the guest's request.\n",
                asked, KLOVRP_LAYOUT_STEREO, asked);
    return KLOVRP_LAYOUT_STEREO;
}

// The size we recommend for an eye layer at a given textureScale, and the ONE
// place that answers it — klovrp_fill_eye_desc and
// klovrp_GetLayerRecommendedResolution both come through here. The
// KL_OVRP_EYE_MAX cap (kl_ovrp_cap_eye_size) applies first;
// KL_OVRP_EYE_CAP_ALLOC (default 0) then bounds the ALLOCATION — the asked
// width times the canted-display union widening — against the same cap.
static void klovrp_eye_desc_size(float texture_scale, int *out_w, int *out_h) {
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    if (!(texture_scale > 0.0f)) texture_scale = 1.0f;
    // KL_OVRP_EYE_SCALE_MIN: a FLOOR under the guest's render scale, before
    // the cap (the ceiling) below. See kl_ovrp_eye_scale_floor.
    {
        float asked = texture_scale;
        texture_scale = kl_ovrp_eye_scale_floor(asked);
        if (texture_scale != asked) {
            static float said_for;
            if (said_for != asked) {
                said_for = asked;
                int aw = (int)((float)w * asked), ah = (int)((float)h * asked);
                int gw = (int)((float)w * texture_scale), gh = (int)((float)h * texture_scale);
                kl_ovrp_cap_eye_size(&gw, &gh);
                fprintf(stderr, "  [ovrp] eye scale clamped: guest asked %dx%d "
                                "(render scale %.3f), gave %dx%d (KL_OVRP_EYE_SCALE_MIN "
                                "%.3f, then the cap) — the eye swapchain keeps "
                                "its size across the guest's per-level scale\n",
                        aw, ah, (double)asked, gw, gh, (double)texture_scale);
            }
        }
    }
    w = (int)((float)w * texture_scale);
    h = (int)((float)h * texture_scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // ...and then capped, by the same ceiling the advertisement answers to.
    // This is the seam the guest ALLOCATES from, so it is the one that has to
    // hold; see kl_ovrp_cap_eye_size for what it cost to learn that. Said once
    // per distinct clamp, because a guest that keeps its render scale asks
    // every frame setup.
    {
        int cw = w, ch = h;
        kl_ovrp_cap_eye_size(&cw, &ch);
        if (cw != w || ch != h) {
            static int said_for;
            if (said_for != w) {
                said_for = w;
                fprintf(stderr, "  [ovrp] eye desc capped: the guest's render "
                                "scale %.3f asked for %dx%d, KL_OVRP_EYE_MAX "
                                "holds it to %dx%d (%.2fx fewer pixels). A "
                                "render scale is supersampling above the "
                                "platform's recommendation, and this host's "
                                "recommendation is already the cap\n",
                        (double)texture_scale, w, h, cw, ch,
                        ((double)w * h) / ((double)cw * ch));
            }
            w = cw; h = ch;
        }
    }

    // Read once — an Unreal guest asks for a desc every frame.
    static int on = -1, cap;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_EYE_CAP_ALLOC", 0);
        cap = kl_env_int("KL_OVRP_EYE_MAX", 3072);
    }
    if (on && cap > 0) {
        float k = klovrp_union_widen();
        float alloc_w = (float)w * k;
        float longest = alloc_w > (float)h ? alloc_w : (float)h;
        if (longest > (float)cap) {
            float s = (float)cap / longest;
            int nw = (int)((float)w * s + 0.5f);
            int nh = (int)((float)h * s + 0.5f);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            // Once per DISTINCT arrival, not once per change: an Unreal guest
            // alternates a scale-1.000 desc and a scale-1.300 one every frame,
            // so "changed since last time" is true on every single call and
            // says the same two things 900 times. A small remembered set is
            // what makes both lines appear exactly once.
            static int seen[4][4], n_seen;
            int now[4] = { w, h, nw, nh };
            int fresh = 1;
            for (int i = 0; i < n_seen; i++)
                if (memcmp(seen[i], now, sizeof now) == 0) { fresh = 0; break; }
            if (fresh) {
                if (n_seen < 4) memcpy(seen[n_seen++], now, sizeof now);
                fprintf(stderr,
                        "  [ovrp] KL_OVRP_EYE_CAP_ALLOC: recommending %dx%d "
                        "rather than %dx%d — the guest widens by %.4f for the "
                        "canted cones and would have allocated %.0f wide "
                        "against a cap of %d (scale %.3f). The FOV is "
                        "unchanged; the angular density is what drops\n",
                        nw, nh, w, h, (double)k, (double)alloc_w, cap,
                        (double)texture_scale);
            }
            w = nw; h = nh;
        }
    }
    *out_w = w;
    *out_h = h;
}

static void klovrp_fill_eye_desc(ovrp_layer_desc_eyefov *d, int layout,
                                 int mip_levels, int sample_count, int format,
                                 int depth_format, int mv_format,
                                 int mv_depth_format,
                                 int layer_flags, float texture_scale) {
    int w = 0, h = 0;
    klovrp_eye_desc_size(texture_scale, &w, &h);

    memset(d, 0, sizeof *d);
    d->shape        = KLOVRP_SHAPE_EYEFOV;
    d->layout       = klovrp_eye_layout(layout);
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
    // ...except the motion-vector SIZE, which is not echoed because nothing
    // echoes it: it is the one field of this desc a real plugin computes, and
    // the Unity provider reads it straight back out as the width and height of
    // the render texture it wraps around whatever
    // ovrp_GetLayerTextureSpaceWarp hands over (measured:
    // OculusDisplayProvider::CreateEyeTextureStages copies desc+0x74/+0x78 into
    // its texture-creation context at +0x14/+0x18, and
    // CreateMotionVectorTexture writes exactly those two into the
    // UnityXRRenderTextureDesc). Left at the memset's zero it is a 0x0 render
    // texture, which is the same failure as no texture at all.
    //
    // A QUARTER PER AXIS, which is Meta's own ratio for an Application
    // SpaceWarp motion-vector buffer (a sixteenth of the pixels). The number is
    // free — we allocate the image on the other side of this seam, so the two
    // agree whatever it is — and picking the shipped ratio rather than
    // something smaller keeps the guest's motion-vector pass the shape it was
    // authored for, at ~8 MB a stage instead of ~130.
    //
    // Only when the layer actually asked for the allocation. A guest with
    // SpaceWarp off never passes the flag and must see the zero a real plugin
    // leaves there.
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
    // First and on change, like every other per-frame report in this file. An
    // UNREAL guest asks this question EVERY FRAME — one device log was
    // thousands of identical `layout=3 5241x4204 … scale=1.300/1.000` lines,
    // which is most of the file and pushes the lines that matter out of a
    // reader's reach. The event is the answer changing; the repetition is not.
    {
        static int have_last;
        static int last[9];
        int now[9] = { layout, out->texture_size.w, out->texture_size.h,
                       mip_levels, sample_count, format, depth_format,
                       mv_format, layer_flags };
        if (have_last && memcmp(last, now, sizeof last) == 0) return OVRP_SUCCESS;
        have_last = 1;
        memcpy(last, now, sizeof last);
    }
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
// textureSize is therefore a POINTER, not a packed pair passed by value. Read
// as a value it yields a SIZE the guest then builds a layer out of: for Unity
// that costs nothing visible, since its only non-eye layer really is a 1x1 it
// never renders into, but for RE4 it is the intro logos — a splash quad told it
// was 1x1.
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
    // Once per distinct request, not once per call, and the difference is 180
    // lines a session. OVROverlay retries a refused layer FOREVER — measured at
    // ~1.5/s from level 3 on, 180 EnqueueSetupLayer2 against 2 DestroyLayer —
    // and it is not being stubborn, it genuinely cannot stop: it discards
    // EnqueueSetupLayer2's return value entirely (libil2cpp 0x1446c38 loads the
    // result and immediately overwrites w0), and the only state that ends the
    // retry is a layerId >= 1 read back out of the pinned handle. There is no
    // failure code, no descriptor shape and no stage count that makes the
    // managed side conclude "unsupported" — it never asks.
    //
    // So the native retry cadence stays until there is a real swapchain to
    // answer with (the shortcut of handing back a positive id is WORSE: the
    // guest then walks into Graphics.Blit against a null swapchain, which
    // targets the currently bound render target — the eye image). What does
    // not need to stay is this log line firing at the same 1.5 Hz forever,
    // which was the only cost the loop actually imposed.
    static int said;
    static int last[8];
    int now[8] = { shape, layout, sz.w, sz.h, out->mip_levels, sample_count,
                   format, layer_flags };
    if (!said || memcmp(last, now, sizeof now) != 0) {
        said = 1;
        memcpy(last, now, sizeof now);
        fprintf(stderr, "  [ovrp] CalculateLayerDesc: shape=%d layout=%d %dx%d "
                        "mips=%d samples=%d fmt=%d flags=%#x — described; the "
                        "layer itself will be declined, and the guest will keep "
                        "asking\n",
                shape, layout, sz.w, sz.h, out->mip_levels, sample_count,
                format, (unsigned)layer_flags);
    }
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
    //  [cp] unwarp grid... for a 3686x2956 eye texture (viewport 0,0 3072x2464)
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
        // Table full and nothing freed. AC Nexus re-creates its overlay set on
        // every loading phase without ever destroying the old layers (measured:
        // zero DestroyLayer/EnqueueDestroyLayer calls), so a hard failure here
        // meant the guest could not finish setting up and the render loop never
        // reached ovrp_EndFrame4 - the eye froze on frame 1. Recycle the OLDEST
        // non-eye layer (the earliest phase's orphans go first; the current
        // menu's newest layers are kept) so SetupLayer never fails. The eye
        // layer is never evicted.
        struct klovrp_layer *oldest = NULL;
        for (int i = 0; i < KLOVRP_MAX_LAYERS; i++) {
            struct klovrp_layer *c = &g_layers[i];
            if (c->is_eye || !c->id) continue;
            if (!oldest || c->id < oldest->id) oldest = c;
        }
        if (!oldest) {
            fprintf(stderr, "  [ovrp] SetupLayer: all %d slots are eye layers - "
                            "cannot recycle\n", KLOVRP_MAX_LAYERS);
            return OVRP_FAIL_INVALID_PARAM;
        }
        static unsigned evicted;
        if (evicted++ % 64 == 0)
            fprintf(stderr, "  [ovrp] SetupLayer: table full, recycling oldest "
                            "non-eye layer %d (%u evictions so far)\n",
                    oldest->id, evicted);
        l = oldest;
        memset(l, 0, sizeof *l);
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
    // Make the reuse REAL on the Vulkan path. The slot's GL names survive in
    // l->tex, but a Vulkan guest's storage lives in kl_vulkan's layer-image
    // table, keyed by the id — which just changed. Left alone, the new id
    // allocates a fresh, empty VkImage, the guest submits it before it has
    // drawn a pixel, and the compositor faithfully composites nothing: wrath2
    // recreates its "compiling shaders" toast on every text update, and each
    // recreate blinked the toast off for the frames the new image spent empty.
    // Re-keying hands the new id the old id's image — still holding the
    // previous picture, which is the exact behaviour the GL half already has.
    if (reused && !is_eye && l->prev_id && kl_vulkan_guest_active())
        kl_vulkan_layer_rekey(l->prev_id, l->id);
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
    kl_loadvid_recount();
    return OVRP_SUCCESS;
}

// ovrp_EnqueueSetupLayer2(desc, compositionDepth, layerId) - the older-ABI
// sibling of ovrp_SetupLayer: the same job (turn a layer desc into an id and a
// slot), with no device handle and a compositionDepth ordering hint this
// compositor does not need - there is one eye layer and one dummy, and their
// z-order is fixed here. AC Nexus's OVRPlugin build enqueues its layers this way
// instead of calling ovrp_SetupLayer. It delegates to the same allocator so the
// two entry points cannot end up describing a layer differently; the device arg
// is only used for SetupLayer's log line, so NULL is fine.
static uint64_t klovrp_EnqueueSetupLayer2(const ovrp_layer_desc_eyefov *desc,
                                          int composition_depth, int *layer_id) {
    ovrp_hit("ovrp_EnqueueSetupLayer2");
    (void)composition_depth;
    // ENABLED again, so AC Nexus's UI/menu OVROverlays (Quad layers) get a real
    // swapchain and composite. Its video-background overlays still end up with
    // uninitialised VkImage handles (no Android Surface here), but those no longer
    // crash: the kl_vulkan image registry refuses any command handed a handle no
    // create produced. So the UI shows and the video background is simply absent.
    return klovrp_SetupLayer(NULL, desc, layer_id);
}

// ovrp_EnqueueSubmitLayer2 - THE per-frame overlay submit AC Nexus uses (~28k
// calls/run, ~7-8 layers every frame). It was a benign no-op, so every overlay
// the guest drew - its entire menu - was silently dropped and never composited,
// which is why the eye stayed black while the game ran fine. Signature is
// OVRPlugin 1.x, all args in x0-x7 on arm64:
//   (uint flags, void* texL, void* texR, int layerId, int frameIndex,
//    const ovrpPosef* pose, const ovrpVector3f* scale, int layerIndex)
// Record each submitted non-eye layer into a per-frame set and publish it to the
// compositor at the frame boundary. The boundary is detected two ways (a changed
// frameIndex, or the same layer id appearing again) so it is robust even if the
// frameIndex arg is not what this ABI guess assumes.
static kl_ovrp_overlay g_enq[KLOVRP_MAX_LAYERS];
static int g_enq_n;
static int g_enq_frame = -0x7fffffff;
static void klovrp_enq_flush(void) {
    if (g_enq_n) kl_ovrp_overlays_external(g_enq, g_enq_n);
    g_enq_n = 0;
}
static uint64_t klovrp_EnqueueSubmitLayer2(uint32_t flags, void *texL, void *texR,
                                           int layer_id, int frame_index,
                                           const float *pose, const float *scale,
                                           int layer_index) {
    ovrp_hit("ovrp_EnqueueSubmitLayer2");
    // Log the SUBMITTED texture handles per layer (once each): if texL differs
    // from the VkImage we handed out via GetLayerTexture2 for this layer, the
    // guest is drawing its UI into ITS OWN texture and we are sampling an empty
    // swapchain image — which would leave every panel composited black.
    { static int seen[128]; static int nseen;
      int known = 0; for (int i=0;i<nseen;i++) if (seen[i]==layer_id) { known=1; break; }
      if (!known && nseen < 128) { seen[nseen++]=layer_id;
        fprintf(stderr, "  [ovrp] EnqueueSubmit texL: layer=%d texL=%#llx texR=%#llx\n",
                layer_id, (unsigned long long)(uintptr_t)texL,
                (unsigned long long)(uintptr_t)texR); } }
    (void)texL; (void)texR;
    static int logged;
    if (logged < 24) {
        logged++;
        struct klovrp_layer *ll = klovrp_layer(layer_id);
        fprintf(stderr, "  [ovrp] EnqueueSubmitLayer2: layer=%d (%s) frame=%d "
                "idx=%d flags=%#x pos=[%.2f %.2f %.2f] scale=[%.2f %.2f %.2f]\n",
                layer_id, ll ? klovrp_shape_name(ll->desc.shape) : "?",
                frame_index, layer_index, flags,
                pose ? pose[4] : 0.f, pose ? pose[5] : 0.f, pose ? pose[6] : 0.f,
                scale ? scale[0] : 0.f, scale ? scale[1] : 0.f, scale ? scale[2] : 0.f);
    }
    int dup = 0;
    for (int i = 0; i < g_enq_n; i++) if (g_enq[i].layer_id == layer_id) { dup = 1; break; }
    if (dup || frame_index != g_enq_frame) {
        klovrp_enq_flush();
        g_enq_frame = frame_index;
    }
    struct klovrp_layer *l = klovrp_layer(layer_id);
    if (l && !l->is_eye && g_enq_n < KLOVRP_MAX_LAYERS) {
        kl_ovrp_overlay *o = &g_enq[g_enq_n++];
        memset(o, 0, sizeof *o);
        o->layer_id = layer_id;
        o->shape    = l->desc.shape;
        o->stage    = 0;   // these layers are set up single-buffered at stage 0
        o->tex_w    = l->desc.texture_size.w;
        o->tex_h    = l->desc.texture_size.h;
        if (pose)  memcpy(o->pose, pose, sizeof o->pose);
        if (scale) { o->size[0] = scale[0]; o->size[1] = scale[1]; }
        o->flags       = (int)flags;
        o->head_locked = ((int)flags & KLOVRP_SUBMIT_HEAD_LOCKED) != 0;
        o->origin_top_left = kl_vulkan_guest_active();
        o->tex = (unsigned long long)(uintptr_t)texL;
    }
    // Diagnostic: every ~600 submits, dump the layer images' lit counts so we
    // can see if the guest is drawing UI into them or submitting empties.
    static unsigned cap;
    if (++cap % 600 == 0) kl_vulkan_capture_layers();
    return OVRP_SUCCESS;
}

// ovrp_SetOverlayQuad3 - the LEGACY per-frame quad-overlay submit from before
// the Enqueue* ABI. Its CAPI return is ovrpBool, so 1 is "overlay applied". AC
// Nexus reaches it once; with no handler the synthetic plugin aborts by name,
// which is the signal-6 crash in this run. We deliberately do NOT decode its
// arguments: the classic signature passes an ovrpPosef (28 bytes, > 16 so passed
// indirectly) and an ovrpVector3f BY VALUE, and the exact packing is version-
// sensitive - reading it wrong is its own crash. A no-argument handler leaves
// every incoming arg untouched in its register and answers success, so the
// guest's OVROverlay path does not disable itself. If a later full-menu run
// shows this quad carries a panel we need, it can be routed into g_enq then.
static uint64_t klovrp_SetOverlayQuad3(void) {
    ovrp_hit("ovrp_SetOverlayQuad3");
    static int once; if (!once) { once = 1;
        fprintf(stderr, "  [ovrp] SetOverlayQuad3: legacy quad overlay acknowledged "
                "(args not decoded) -> ovrpBool true\n"); }
    return OVRP_TRUE;                       // ovrpBool
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
    if (l) { l->prev_id = l->id; l->id = 0; }
    kl_loadvid_recount();
    return OVRP_SUCCESS;
}

// ovrp_EnqueueDestroyLayer(layerId) - the Enqueue-ABI sibling of DestroyLayer,
// the teardown half of ovrp_EnqueueSetupLayer2. AC Nexus's menu drives its
// OVROverlay panels this way: EnqueueSetupLayer2 to make one, EnqueueDestroyLayer
// to free it. Without this handler the destroy was an unimplemented no-op, so
// every panel the game freed kept its slot forever and the 32-slot table filled
// in seconds - SetupLayer then failed for the rest of the run and the render
// loop stalled. Frees the slot the same careful way DestroyLayer does (the
// textures are kept for reuse, only the id is cleared).
static uint64_t klovrp_EnqueueDestroyLayer(int layer_id) {
    ovrp_hit("ovrp_EnqueueDestroyLayer");
    return klovrp_DestroyLayer(layer_id);
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
    } else if (l->is_eye && l->desc.layout != KLOVRP_LAYOUT_STEREO &&
               l->desc.layout != KLOVRP_LAYOUT_DOUBLEWIDE) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] GetLayerTexture2: layer %d asks for layout %d "
                            "(%s); only Stereo, DoubleWide and (Vulkan) Array are "
                            "implemented — refusing rather than binding the wrong "
                            "storage\n", layer_id, l->desc.layout,
                    l->desc.layout == KLOVRP_LAYOUT_ARRAY ? "Array" : "?");
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

    // DoubleWide (TWD2, UE4 GLES): both eyes are halves of ONE wide texture. Key
    // the storage on a canonical eye (0) so a single texture is allocated, then
    // register it under BOTH (eye, stage) keys below — the compositor already
    // crops each eye by its own submit viewport (left half / right half), so a
    // shared image plus the per-eye viewports it receives is the whole of it.
    int canon = (l->is_eye && l->desc.layout == KLOVRP_LAYOUT_DOUBLEWIDE) ? 0 : eye;
    uint32_t *slot = &l->tex[stage][canon];
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
        // The storage IS a compositor MTLTexture when one is on offer, exactly
        // as in klovrp_SetupEyeTexture2. Which of the two storages the name got
        // is a branch INSIDE the creation, never a second way out of this
        // function: *color must be written on every path. An early return hands
        // the guest its own uninitialised stack slot as a texture name — a
        // garbage attachment, GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, and an eye
        // texture nothing renders into, only when a provider is registered (a
        // host run without one stays correct, so it reads as a compositor bug).
        int mtl_backed = 0;
        if (l->is_eye) {
            kl_glfb_note_eye_texture(canon, stage, name);
            mtl_backed = kl_glfb_has_mtl_provider() &&
                         kl_glfb_bind_eye_mtl_texture(canon, stage, name, w, h, glfmt);
            // DoubleWide: the guest fetches ONLY eye 0 (one wide texture holds both
            // eyes), so eye 1 is never requested and the compositor would bind
            // nothing for the right eye — which shows through as passthrough. Register
            // the same wide texture for eye 1 now; the per-eye submit viewport crops
            // each eye to its half.
            if (l->desc.layout == KLOVRP_LAYOUT_DOUBLEWIDE) {
                l->tex[stage][1] = name;
                kl_glfb_note_eye_texture(1, stage, name);
                if (kl_glfb_has_mtl_provider())
                    kl_glfb_bind_eye_mtl_texture(1, stage, name, w, h, glfmt);
            }
        }
        if (!mtl_backed && gl_BindTexture && gl_TexStorage2D) {
            gl_BindTexture(0x0DE1 /* GL_TEXTURE_2D */, name);
            gl_TexStorage2D(0x0DE1, 1, glfmt, w, h);
        }
        // What the eye storage IS, which is what the reuse in klovrp_SetupLayer
        // reads — see the comment there. kl_glfb keys the eye
        // textures by (eye, stage) and nothing else, so this call has just
        // OVERWRITTEN whatever the previous eye layer put in those slots.
        if (l->is_eye) { g_eye_storage_w = w; g_eye_storage_h = h; }
        fprintf(stderr, "  [ovrp] GetLayerTexture2: %s %d stage %d = GL %u "
                        "(%dx%d %s / GL %#x%s)\n",
                l->is_eye ? "eye" : "dummy layer", eye, stage, name, w, h,
                fname ? fname : "?", glfmt,
                mtl_backed ? ", MTLTexture-backed" : "");
    }
    // DoubleWide: the requested eye differs from the canonical storage eye — make
    // (eye, stage) point at the same wide texture so the compositor binds it for
    // this eye too and crops it by this eye's viewport.
    if (canon != eye) {
        l->tex[stage][eye] = *slot;
        kl_glfb_note_eye_texture(eye, stage, *slot);
        if (kl_glfb_has_mtl_provider()) {
            const char *fn = NULL;
            uint32_t gf = klovrp_gl_format(l->desc.format, &fn);
            if (!gf) gf = KL_OVRP_TEXFMT_EYE;
            kl_glfb_bind_eye_mtl_texture(eye, stage, *slot,
                l->desc.texture_size.w, l->desc.texture_size.h, gf);
        }
        fprintf(stderr, "  [ovrp] GetLayerTexture2: DoubleWide eye %d stage %d shares "
                        "wide texture %u with eye 0\n", eye, stage, *slot);
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

// ovrp_GetLayerTextureSpaceWarp(layerId, stage, eye, uint64_t* motion,
//                               uint64_t* depth) - the Application SpaceWarp
// motion-vector + depth textures, GetLayerTextureFoveation's exact shape and
// exact answer. ASW is frame extrapolation driven by motion vectors the GUEST
// renders; nothing on this host consumes them - the reprojection here is the
// visionOS compositor's, below the guest - so there is no texture to hand out.
// The caller tests the result (cbnz w0) and, on failure, does not enable
// SpaceWarp and carries on; SUCCESS with zero handles would be the damaging
// answer, flagging the descriptor as space-warped against texture id 0. Same
// story we already tell for foveation and ovrp_GetTiledMultiResSupported.
static uint64_t klovrp_GetLayerTextureSpaceWarp(int layer_id, int stage, int eye,
                                                uint64_t *motion, uint64_t *depth) {
    ovrp_hit("ovrp_GetLayerTextureSpaceWarp");
    (void)layer_id; (void)stage; (void)eye;
    if (!motion || !depth) return OVRP_FAIL_INVALID_PARAM;
    *motion = 0;
    *depth = 0;
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_GetLayerTexturePtr(layerId, stage, eye, uint64_t* texturePtr) - the
// legacy single-handle getter the guest uses for its non-eye layers (AC Nexus's
// Equirect background). It is ovrp_GetLayerTexture2 with one out-param instead
// of a colour/depth pair, so it delegates: GetLayerTexture2 already keys non-eye
// layers by layer_id (not the shared eye storage), so this allocates the layer's
// own image and cannot disturb the eyes. The handle it hands back is the same
// 64-bit value - a GL name on the GLES path, a VkImage on Vulkan.
static uint64_t klovrp_GetLayerTexturePtr(int layer_id, int stage, int eye,
                                          uint64_t *texture_ptr) {
    ovrp_hit("ovrp_GetLayerTexturePtr");
    uint64_t color = 0, depth = 0;
    uint64_t r = klovrp_GetLayerTexture2(layer_id, stage, eye, &color, &depth);
    if (texture_ptr) *texture_ptr = color;
    // TEMP diagnostic: the guest was seen creating an image view on a wild
    // pointer right after this call - if these args do not read as a plausible
    // (layer, stage, eye, ptr) then the signature is wrong and the real texture
    // out-param is being left uninitialised.
    fprintf(stderr, "  [ovrp] GetLayerTexturePtr(layer=%d stage=%d eye=%d ptr=%p) "
                    "wrote color=%#llx depth=%#llx result=%llu\n",
            layer_id, stage, eye, (void *)texture_ptr,
            (unsigned long long)color, (unsigned long long)depth,
            (unsigned long long)r);
    return r;
}

// ovrp_GetLayerAndroidSurfaceObject(layerId, void** surfaceObject) - hands back
// a Java Surface for an ANDROID SURFACE layer, which is how a title renders a
// video straight onto an OVROverlay (AC Nexus's looping background). There is no
// Android Surface / MediaPlayer surface path here, so it is refused - and the
// out-param is ZEROED, not left alone: an unwritten jobject is the exact wild
// pointer the kl_vulkan image-view guard now catches, one layer up. The caller
// tests the result and, on failure, simply does not attach a video surface and
// carries on; the background is dropped, the scene is not.
static uint64_t klovrp_GetLayerAndroidSurfaceObject(int layer_id, void **surface) {
    ovrp_hit("ovrp_GetLayerAndroidSurfaceObject");
    (void)layer_id;
    if (surface) *surface = NULL;
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
// The read-back answers what the setter stored rather than a constant: a getter
// that disagrees with the setter means the guest sets a level, reads a different
// one back, and concludes the request was rejected. The un-suffixed setters go
// through the same store, so there is one value and not two.
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

// The un-suffixed CPU/GPU level getters return the level DIRECTLY (the ...2 form
// writes it through an out-param). Liminal's Oculus Utilities 1.40 calls these
// on content load; without them the run aborted on the unimplemented entry.
static uint64_t klovrp_GetSystemCpuLevel(void) {
    ovrp_hit("ovrp_GetSystemCpuLevel");
    return (uint64_t)(uint32_t)g_cpu_level;
}
static uint64_t klovrp_GetSystemGpuLevel(void) {
    ovrp_hit("ovrp_GetSystemGpuLevel");
    return (uint64_t)(uint32_t)g_gpu_level;
}

// ---------------------------------------------------------------------------
// The compositor-telemetry group
// ---------------------------------------------------------------------------
// 1.40's plugin publishes Unity's XR stats every frame, which means asking
// OVRPlugin how long the compositor took. There is no compositor here in the
// sense these questions mean — no VrApi frame submission with a measured GPU
// end and a vsync to be early or late for — so every one of them is refused
// rather than answered with a plausible number. This is a GROUP answer on
// purpose: a per-call mix of invented milliseconds would let
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

// ---------------------------------------------------------------------------
// Insight Passthrough — the seventeen `ovrp_*Passthrough*` entry points.
//
// UE5.3's OculusXR HMD calls `ovrp_InitializeInsightPassthrough` from its
// startup, and Pinball FX stopped there. This is the headset's colour
// see-through camera feed, composited by the RUNTIME into its own layer types
// (ovrpLayerType 6/7 and the two keyboard-hands ones this file already names).
// There is no camera here and no layer type behind it, so the whole family
// answers "not here" — and the answers are shaped so the guest's own uninit
// path is the one it takes:
//
//   IsInsightPassthroughSupported(ovrpBool *out) — the real one (0x182080)
//       writes 0 to *out FIRST and then overwrites it, so a false with SUCCESS
//       is a value it produces itself. This is the query UE gates on.
//   InitializeInsightPassthrough() — refused. The engine's caller records the
//       failure and leaves bPassthroughInitialized false.
//   GetInsightPassthroughInitializationState() — -1002, byte for byte what the
//       real plugin returns (0x182268) with no passthrough object built.
//   GetInsightPassthroughInitialized() — a bare ovrpBool, false.
//   ShutdownInsightPassthrough() — success: tearing down nothing succeeds, and
//       UE calls it unconditionally on the way out.
//   The capability / preference getters — -1002, the same uninitialized answer
//       the real ones give (0x182520, 0x18278c).
//
// NOTE this is the ONE family in this file that answers -1002 rather than
// klovrp_refuse's -1000, and the reason is the inverse of the note above
// klovrp_refuse: there we are initialized and must not say otherwise, here the
// PASSTHROUGH subsystem genuinely is not initialized and the real plugin says
// exactly this when it is not. The two do not contradict.
//
// Everything that acts on a passthrough object it could never have been given —
// geometry, styles, colour LUTs — is refused through klovrp_refuse.
static uint64_t klovrp_IsInsightPassthroughSupported(int *out) {
    ovrp_hit("ovrp_IsInsightPassthroughSupported");
    if (!out) return OVRP_FAILURE;
    *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_InitializeInsightPassthrough(void) {
    ovrp_hit("ovrp_InitializeInsightPassthrough");
    return OVRP_FAIL_UNSUPPORTED;
}
static uint64_t klovrp_ShutdownInsightPassthrough(void) {
    ovrp_hit("ovrp_ShutdownInsightPassthrough");
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetInsightPassthroughInitialized(void) {
    ovrp_hit("ovrp_GetInsightPassthroughInitialized");
    return 0;
}
static uint64_t klovrp_GetInsightPassthroughInitializationState(void) {
    ovrp_hit("ovrp_GetInsightPassthroughInitializationState");
    return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
}
static uint64_t klovrp_GetPassthroughCapabilityFlags(void *out) {
    ovrp_hit("ovrp_GetPassthroughCapabilityFlags");
    (void)out;
    return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
}
static uint64_t klovrp_GetPassthroughCapabilities(void *out) {
    ovrp_hit("ovrp_GetPassthroughCapabilities");
    (void)out;
    return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
}
static uint64_t klovrp_GetPassthroughPreferences(void *out) {
    ovrp_hit("ovrp_GetPassthroughPreferences");
    (void)out;
    return (uint64_t)(int64_t)OVRP_FAIL_NOT_INITIALIZED;
}
#define KLOVRP_PASSTHROUGH_REFUSE(fn, name)                     \
    static uint64_t fn(void) { return klovrp_refuse(name); }
KLOVRP_PASSTHROUGH_REFUSE(klovrp_AddInsightPassthroughSurfaceGeometry,
                          "ovrp_AddInsightPassthroughSurfaceGeometry")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_DestroyInsightPassthroughGeometryInstance,
                          "ovrp_DestroyInsightPassthroughGeometryInstance")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_UpdateInsightPassthroughGeometryTransform,
                          "ovrp_UpdateInsightPassthroughGeometryTransform")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_SetInsightPassthroughStyle,
                          "ovrp_SetInsightPassthroughStyle")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_SetInsightPassthroughStyle2,
                          "ovrp_SetInsightPassthroughStyle2")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_SetInsightPassthroughKeyboardHandsIntensity,
                          "ovrp_SetInsightPassthroughKeyboardHandsIntensity")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_CreatePassthroughColorLut,
                          "ovrp_CreatePassthroughColorLut")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_DestroyPassthroughColorLut,
                          "ovrp_DestroyPassthroughColorLut")
KLOVRP_PASSTHROUGH_REFUSE(klovrp_UpdatePassthroughColorLut,
                          "ovrp_UpdatePassthroughColorLut")
#undef KLOVRP_PASSTHROUGH_REFUSE

// ---------------------------------------------------------------------------
// QPL — the ten `ovrp_Qpl*` entry points. UE5.3's Meta fork calls
// `ovrp_QplMarkerStart` from engine init, which is where Pinball FX stopped.
//
// This is Facebook's `perflogger` (`facebook::perflogger::QPL`) reached through
// the plugin: a marker / annotation stream bound for Meta's analytics pipeline.
// It is WRITE-ONLY telemetry — nothing the guest reads back from it changes its
// behaviour — and there is no pipeline here to write to, so every one of these
// is a success no-op. That is an honest answer rather than a stub: a headset
// with telemetry consent withheld runs exactly this way.
//
// The ABI is taken off this APK's own libOVRPlugin rather than guessed. All ten
// wrappers share one tail — `tst w0, #1; csel w0, wzr, w8, ne` with w8 = -1000
// — so success is 0 and the failure is plain ovrpFailure. Only ONE writes
// through a pointer:
//
//   ovrp_QplCreateMarkerHandle(const char *name, int *out_handle)
//       0x26f9e0: `cbz x1` -> false; otherwise a file-static counter is
//       incremented and stored to [x1]. The handle is a monotonic id and
//       nothing else, which is what is reproduced here.
//
// ovrp_QplSetConsent is the odd one: it narrows w0 to a bool and then returns
// **0 unconditionally** — it cannot fail.
static uint64_t klovrp_QplMarkerStart(int marker, int instance, int64_t ts) {
    ovrp_hit("ovrp_QplMarkerStart");
    (void)marker; (void)instance; (void)ts;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerEnd(int marker, short status, int instance, int64_t ts) {
    ovrp_hit("ovrp_QplMarkerEnd");
    (void)marker; (void)status; (void)instance; (void)ts;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerPoint(int marker, const char *name, int instance,
                                      int64_t ts) {
    ovrp_hit("ovrp_QplMarkerPoint");
    (void)marker; (void)name; (void)instance; (void)ts;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerPointCached(int marker, int name_handle, int instance,
                                            int64_t ts) {
    ovrp_hit("ovrp_QplMarkerPointCached");
    (void)marker; (void)name_handle; (void)instance; (void)ts;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerPointData(int marker, const char *name, void *data,
                                          int count, int instance, int64_t ts) {
    ovrp_hit("ovrp_QplMarkerPointData");
    (void)marker; (void)name; (void)data; (void)count; (void)instance; (void)ts;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerAnnotation(int marker, const char *key,
                                           const char *value, int instance) {
    ovrp_hit("ovrp_QplMarkerAnnotation");
    (void)marker; (void)key; (void)value; (void)instance;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplMarkerAnnotationVariant(int marker, const char *key,
                                                  const void *variant, int instance) {
    ovrp_hit("ovrp_QplMarkerAnnotationVariant");
    (void)marker; (void)key; (void)instance;
    // The real wrapper returns ovrpFailure on a NULL variant (`cbz x2`), and
    // that pointer is the one argument it dereferences.
    if (!variant) return OVRP_FAILURE;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplCreateMarkerHandle(const char *name, int *out) {
    ovrp_hit("ovrp_QplCreateMarkerHandle");
    (void)name;
    if (!out) return OVRP_FAILURE;
    static int next_handle;
    *out = ++next_handle;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplDestroyMarkerHandle(int handle) {
    ovrp_hit("ovrp_QplDestroyMarkerHandle");
    (void)handle;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_QplSetConsent(int consent) {
    ovrp_hit("ovrp_QplSetConsent");
    (void)consent;
    return OVRP_SUCCESS;   // the real one cannot fail
}

static uint64_t klovrp_GetPerfMetricsInt(int metric, int *out) {
    ovrp_hit("ovrp_GetPerfMetricsInt");
    (void)metric;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_FAIL_UNSUPPORTED;
}

// ovrp_GetViewportStencil(eye, type, ovrpVector2f* verts, int* vertexCount,
//                        uint16_t* indices, int* indexCount) — real 0x171bf0,
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
// here too, or every transition leaks a whole swapchain.
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

// A zeroed struct is the complete honest answer — no perf stats, because there
// is no compositor producing them. What took a second reading is HOW it is
// handed back. The real 1.59 entry point returns the 292-byte ovrpAppPerfStats
// BY VALUE (superhot's libOVRPlugin.so, 0x27d00):
//
//   27d14:  mov  x19, x8              ; the caller's indirect-result buffer
//   27d18:  bl   ovrp_GetAppPerfStats2
//   27d24:  mov  w2, #0x124           ; 292
//   27d28:  mov  x0, x19
//   27d2c:  bl   memcpy               ; ...which returns dest, so x0 == x8 too
//   27d40:  (failure arm) memset(x19, 0, 0x124)
//
// so every caller passes x8, and x0 on return is that same pointer. Beat
// Saber's libunity reading `[x0+0x118]` after the call (0x9bcc58) is therefore
// not "it wants a pointer back" — it is the ordinary sret pattern, read through
// the pointer memcpy handed back. Returning our own static satisfied that
// reader by accident; a caller that reads its OWN buffer instead, which is what
// the C prototype produces, got uninitialised stack every frame.
//
// Writing the caller's buffer AND returning it is a strict superset of both, and
// is exactly what the real function does. The static stays as the answer for a
// caller that passed no buffer at all, which is the one case where writing
// through x8 would be a wild store.
#define KL_OVRP_PERFSTATS_BYTES 0x124        // 292, from the memcpy above
uint64_t klovrp_GetAppPerfStats_impl(void *out) {   // extern: the .S thunk calls it
    static char stats[KL_OVRP_PERFSTATS_BYTES];
    ovrp_hit("ovrp_GetAppPerfStats");
    if (!out) return (uint64_t)(uintptr_t)stats;
    memset(out, 0, KL_OVRP_PERFSTATS_BYTES);
    return (uint64_t)(uintptr_t)out;
}

// Optional depth-compositing probe (slot null-checked, 0x9bced4). We do not
// composite depth, so both the return and the out-int say so.
static uint64_t klovrp_GetDepthCompositingSupported(int *out) {
    ovrp_hit("ovrp_GetDepthCompositingSupported");
    *out = 0;
    return 0;
}

// Eye tracking: the guest asks whether the runtime can hand it a per-eye gaze
// vector. Vision Pro deliberately never exposes gaze to a guest process, so the
// honest and safe answer is "not supported" — the guest then skips its
// eye-tracked foveation / gaze paths entirely instead of calling
// ovrp_GetEyeGazesState and dereferencing a struct we never fill. Takes an
// ovrpBool* out and returns ovrpResult (0 = success), so *out=0 is FALSE.

// Mixed-reality capture — the camera composite an Oculus device does for
// spectators. It takes no arguments and returns `ovrpBool`, not `ovrpResult`, so
// 0 is FALSE and is the intended answer — worth saying out loud, because the two
// types disagree about which value means yes and 0 looks like the wrong one
// here. There is no capture camera, `ovrp_InitializeMixedReality` is
// never answered, and the guest polls this before deciding whether to build the
// composite path at all.
static uint64_t klovrp_GetMixedRealityInitialized(void) {
    ovrp_hit("ovrp_GetMixedRealityInitialized");
    return 0;
}

// Unity calls this on every native plugin it loads, handing over its
// IUnityInterfaces registry. The real OVRPlugin uses it to grab
// IUnityGraphicsVulkan/GLES; we record it and do nothing, which is correct until
// there is a renderer to bind to.
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
// It is an array of STRING POINTERS, not a character buffer. Read the other way
// it is a `strlen` of whatever the caller's uninitialised slot happened to hold,
// on a thread whose crash report names nothing. The two readings are
// indistinguishable from the real plugin alone:
//
//   0x126f50 <ovrp_GetInstanceExtensionsVk>:
//    cbz  x1, +0x28        -> mov w0, #-1001   (invalid parameter)
//     ...initialized?       -> mov w0, #-1003
//     and  w0, w0, w0, asr #31                  (plain ovrpResult)
//
// That says x1 is required and x0 is not checked — consistent with BOTH
// `(char *buf, uint32_t *cap)` and the truth. What settles it is the CALLER,
// whose C++ name survived in libOculusXRPlugin.so:
//
//   OculusSystem::GetVulkanExtensions(void*, unsigned, unsigned*, char*,
//                                    ovrpResult (*)(char const**, int*))
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
//    mov w1, w0 / cmp w1, #3 / b.hi -> -1001     (skeletonType, 0..3)
//     cbz x2 -> -1001                             (the out struct is required)
//     ...no singleton -> -1002
//
// so it is `ovrpResult f(ovrpSkeletonType, ovrpSkeleton2 *)`.
//
// Refused, and deliberately without writing the out struct. Every other
// out-parameter in this file is filled, but that rule cannot be followed here
// honestly: `ovrpSkeleton2`
// is a large struct whose layout is NOT available anywhere in this APK (no DWARF
// for it, and the C# side is IL2CPP'd), so zeroing it would mean guessing a size
// and memset-ing that many bytes of the caller's stack. A guessed size is a
// stack smash; a refusal is a code path the caller already has.
//
// It is safe here because the caller CHECKS: OVRPlugin's C# wrapper is
// `return ovrp_GetSkeleton2(t, out s) == Result.Success`, so a negative result
// is a `false` return and the guest skips its skeleton setup. A guest that
// ignores the result gets garbage bones rather than a crash; the fix then is to
// find the layout, not to invent one.
// The whole family answers the same way, so it is one function rather than one
// per entry point. That normally does not hold — a numbered suffix marks an ABI
// change, so `ovrp_Foo` and `ovrp_Foo2` must not be assumed to share a shape. It
// is safe HERE and only here because this handler
// reads no argument and writes no out-parameter: it returns a constant negative
// whatever the calling convention is, so there is no shape to get wrong.
//
// Each name is still listed explicitly rather than matched by prefix, so a new
// `ovrp_Hand*` in a future SDK lands on the fail-closed abort-by-name and gets
// classified deliberately.
static int32_t klovrp_hand_unsupported(void) { return OVRP_FAIL_UNSUPPORTED; }

// The haptics SDK's parametric extension probe (Deadpool VR's libhaptics_sdk.so
// dlsyms it): its own strings say the expected unsupported answer is
// ovrpFailure_Unsupported, after which parametric haptics are simply "not
// available" and the SDK moves on. Same constant-negative shape as the hand
// family above — no argument is read, so there is no ABI shape to get wrong.
static int32_t klovrp_parametric_unsupported(void) { return OVRP_FAIL_UNSUPPORTED; }

// The rest of the BODY-tracking surface, and the reason it is a family rather
// than the one name that aborted.
//
// Deadpool VR's UOculusBodyTrackingComponent does not stop at GetBodyState4. On
// the gameplay -> main-menu transition (the component being re-created against
// a fresh world) it calls ovrp_RequestBodyTrackingFidelity, which was in no
// list — so it took the unimplemented-entry-point abort at the top of this file
// and the app died with SIGABRT right after printing its OVRPlugin work list —
// a SIGABRT that looked unexplained until the fatal line was found above the
// report in the log.
//
// Listed as the whole family for the same reason as g_ovrp_layer_no: a
// tracking surface is a SEQUENCE (request a fidelity, start, read status,
// re-calibrate, stop), so answering only the name that happened to abort moves
// the abort one call to the right and the next crash names a function that was
// never the problem.
//
// ovrpFailure_Unsupported is the truthful answer and the one this file already
// gives for the same capability through GetBodyTrackingSupported/Enabled
// (g_ovrp_bool_no) and GetBodyState4 — the engine checks the SIGN and puts the
// anim graph on its plain skeleton fallback. The status getters take out
// params they do not fill; that is safe for the same reason GetBodyState4's is
// — a negative result means the caller must not read the buffer, and this
// engine does not.
static int32_t klovrp_body_unsupported(void) { return OVRP_FAIL_UNSUPPORTED; }

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
void klovrp_GetAppPerfStats_entry(void);

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
                    "(the real one NEEDs libvrapi.so)\n", b);
    return (void *)g_ovrp_handle;
}

int kl_ovrp_is_handle(const void *h) { return h == (const void *)g_ovrp_handle; }

// ---------------------------------------------------------------------------
// The second wave: what a UE4.27 guest asks once the eye layer exists
// ---------------------------------------------------------------------------
// FNAF: Help Wanted 2 is the first guest in this project to get past
// FOculusHMD::InitializeSession and into UE4's own stereo-rendering setup, and
// it aborted on ovrp_GetLayerRecommendedResolution — an entry point no Unity
// guest has ever reached, because Unity asks the eye texture size a different
// way (ovrp_GetEyeTextureSize).
//
// Every signature below is READ OFF FNAF: Help Wanted 2's own libOVRPlugin.so
// (1.60.0), not inferred from a header: the thin
// forwarders there shuffle their arguments into the delegate call in a way that
// names the out-pointer's register exactly, and their NULL and not-initialised
// arms name the failure constants. Where a note below says "real 0x…", that is
// the address in that file.
//
// The whole surface the guest RESOLVED was diffed against the surface this file
// ANSWERS, and everything plausibly on the boot-to-menu path is answered here or
// in the class lists below. Answering only the name that aborted is what turns
// a bring-up into one launch per call.

// ovrp_GetLayerRecommendedResolution(int layerId, ovrpSizei* out) — real
// 0x175760: layerId in w0, the out-pointer in x1 (`mov x19, x1`), one 8-byte
// store (`ldr x8, [sp] / str x8, [x19]`), i.e. the two ints of an ovrpSizei.
// -1002 when the plugin is not up, and on a delegate failure it returns the
// negative result WITHOUT touching the out-param.
//
// The recommended resolution is not a new measurement: it is the same per-eye
// render-target size klovrp_fill_eye_desc already hands out through
// ovrp_CalculateEyeLayerDesc3, and the same one ovrp_GetEyeTextureSize answers
// for a Unity guest. Three routes to one number — if this invented a different
// one, UE would size its render target to a resolution the swapchain was not
// built at, and the mismatch would land as a scaled or clipped picture with no
// error anywhere.
//
// A NON-eye layer is answered from its own desc, because that is what the
// question means for a quad: the size that layer was set up at.
//
// **KL_OVRP_LAYER_REC_DISPLAY=1** (default 0; Pinball FX VR needed it)
// answers an EYE layer from klovrp_eye_desc_size at scale 1.000 —
// the same seam every eye desc comes through, so KL_OVRP_EYE_CAP_ALLOC's
// allocation bound applies to this answer too. "Recommended" is a property of
// the panel and the cap, not of the texture the guest most recently asked for;
// with the knob on no engine revision can make one inflated layer justify the
// next (UE5.3 derives its second desc from our recommended size times its own
// 1.300 pixel density). Off, the answer is the measured per-eye size above,
// unchanged for every other target.
static uint64_t klovrp_GetLayerRecommendedResolution(int layer_id, ovrp_sizei *out) {
    ovrp_hit("ovrp_GetLayerRecommendedResolution");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    struct klovrp_layer *l = klovrp_layer(layer_id);
    static int from_display = -1;
    if (from_display < 0) from_display = kl_env_on("KL_OVRP_LAYER_REC_DISPLAY", 0);
    const char *how = "the same per-eye size CalculateEyeLayerDesc reports";
    int w = 0, h = 0;
    if (l && !l->is_eye) {
        w = l->desc.texture_size.w;
        h = l->desc.texture_size.h;
        how = "the size SetupLayer reported";
    } else if (from_display) {
        kl_ovrp_eye_texture_size(&w, &h);
        if (w > 0 && h > 0) klovrp_eye_desc_size(1.0f, &w, &h);
        how = "the DISPLAY's own recommendation, post-cap "
              "(KL_OVRP_LAYER_REC_DISPLAY)";
    } else {
        kl_ovrp_eye_texture_size(&w, &h);
    }
    if (w < 1 || h < 1) return OVRP_FAIL_NOT_INITIALIZED;
    out->w = w;
    out->h = h;
    static int said;
    if (!said++)
        fprintf(stderr, "  [ovrp] GetLayerRecommendedResolution(layer %d) -> %dx%d "
                        "(%s)\n", layer_id, w, h, how);
    return OVRP_SUCCESS;
}

// ...and the eye-layer form beside it, real 0x1757e0: NO layer id, the
// out-pointer in x0, otherwise byte-for-byte the same shape. This guest resolved
// only the first, but the two are one question — a fork that asks the other
// would abort on a name that was never the problem, which is the family rule
// this file has paid for twice already.
static uint64_t klovrp_GetEyeLayerRecommendedResolution(ovrp_sizei *out) {
    ovrp_hit("ovrp_GetEyeLayerRecommendedResolution");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    if (w < 1 || h < 1) return OVRP_FAIL_NOT_INITIALIZED;
    out->w = w; out->h = h;
    return OVRP_SUCCESS;
}

// ovrp_GetHmdToEyeOffset2(ovrpEye eye, ovrpVector3f* out) — real 0x16f830:
// eye in w0, out in x1, three `str s0/s1/s2` and -1001 on a NULL x1.
//
// This is the SAME half-IPD klovrp_GetUserIPD2 measures the separation of, and
// it comes from the one place any eye offset is allowed to come from
// (klovrp_eye_offset) — the frontend's measured offsets, or KL_OVRP_IPD. UE4
// builds its stereo view matrices from this, so a value that disagreed with the
// frustum in ovrp_CalculateEyeLayerDesc is a picture that is subtly wrong in
// both eyes and correct in neither.
static uint64_t klovrp_GetHmdToEyeOffset2(int eye, float *out) {
    ovrp_hit("ovrp_GetHmdToEyeOffset2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    if ((unsigned)eye > 1) return OVRP_FAIL_INVALID_PARAM;
    klovrp_eye_offset(eye, &out[0], &out[1], &out[2]);
    return OVRP_SUCCESS;
}

// ovrp_GetEyePixelsPerTanAngleAtCenter2(ovrpEye eye, ovrpVector2f* out) — real
// 0x16f7c0: eye in w0, out in x1, one `stp s0, s1` and -1001 on NULL.
//
// Derived, not invented: pixels per unit tangent is the eye render target's
// size divided by the tangent extent of the frustum it covers, and both halves
// are already this file's own answers (kl_ovrp_eye_texture_size and
// klovrp_eye_tan, the same pair klovrp_fill_eye_desc uses). UE4 divides by it
// to convert its pixel-density setting into a viewport, so an invented number
// is a viewport that does not match the texture it renders into.
static uint64_t klovrp_GetEyePixelsPerTanAngleAtCenter2(int eye, float *out) {
    ovrp_hit("ovrp_GetEyePixelsPerTanAngleAtCenter2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    if ((unsigned)eye > 1) eye = 0;
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    // g_eye_tan is {left, right, top, bottom} and all four are magnitudes.
    const float *t = klovrp_eye_tan(eye);
    float tan_x = t[0] + t[1], tan_y = t[2] + t[3];
    if (!(tan_x > 0.0f) || !(tan_y > 0.0f) || w < 1 || h < 1)
        return OVRP_FAIL_NOT_INITIALIZED;
    out[0] = (float)w / tan_x;
    out[1] = (float)h / tan_y;
    return OVRP_SUCCESS;
}

// ovrp_GetUserEyeHeight2 (FNAF: Help Wanted 2's real 0x171c10 — the same shape
// as Pinball FX VR's 0x177d60) is defined with the IPD pair near the top of this file, where both
// of its answers and KL_OVRP_USER_EYE_HEIGHT are argued.

// --- Setter/getter pairs -------------------------------------------------
//
// Each of these four was a write-only hint in g_ovrp_result_ok until a guest
// asked to READ it back. A hint that is accepted and then denied is worse than
// one that was refused: the guest sets a performance level, reads a different
// one, and concludes the plugin overrode it. So the setter now RECORDS and the
// getter ECHOES, which is exactly what the real plugin does — there is still
// nothing on this host that acts on any of them.
static int   g_cpu_perf_level = 2;   // ovrpProcessorPerformanceLevel_SustainedHigh
static int   g_gpu_perf_level = 2;
static int   g_app_cpu_priority = 1; // the real plugin's default is "yes"
static int   g_client_color_space;   // ovrpColorSpace_Unknown until a client says

static uint64_t klovrp_SetSuggestedCpuPerformanceLevel(int level) {
    ovrp_hit("ovrp_SetSuggestedCpuPerformanceLevel");
    g_cpu_perf_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSuggestedCpuPerformanceLevel(int *out) {
    ovrp_hit("ovrp_GetSuggestedCpuPerformanceLevel");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_cpu_perf_level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetSuggestedGpuPerformanceLevel(int level) {
    ovrp_hit("ovrp_SetSuggestedGpuPerformanceLevel");
    g_gpu_perf_level = level;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSuggestedGpuPerformanceLevel(int *out) {
    ovrp_hit("ovrp_GetSuggestedGpuPerformanceLevel");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_gpu_perf_level;
    return OVRP_SUCCESS;
}
// Real 0x170f00 masks the delegate's answer with 1 before storing it, so this
// is an ovrpBool out-param and not an integer priority.
static uint64_t klovrp_SetAppCPUPriority2(int on) {
    ovrp_hit("ovrp_SetAppCPUPriority2");
    g_app_cpu_priority = on ? 1 : 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetAppCPUPriority2(int *out) {
    ovrp_hit("ovrp_GetAppCPUPriority2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_app_cpu_priority;
    return OVRP_SUCCESS;
}
// ovrp_SetClientColorDesc(ovrpColorSpace) / ovrp_GetHmdColorDesc(ovrpHmdColorDesc*)
// — real 0x173328 takes the out-pointer in x0 and -1001s on NULL;
// ovrpHmdColorDesc is one ovrpColorSpace field in 4.27, so four bytes.
//
// The answer is whatever the client last DECLARED, because that is the truth
// here: this compositor presents the guest's eye texture as it is and performs
// no colour conversion, so the display's colour space IS the one the client
// asked to work in. Reporting a Quest panel's native space instead would invite
// UE to convert into a gamut nothing downstream applies.
static uint64_t klovrp_SetClientColorDesc(int color_space) {
    ovrp_hit("ovrp_SetClientColorDesc");
    g_client_color_space = color_space;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetHmdColorDesc(int *out) {
    ovrp_hit("ovrp_GetHmdColorDesc");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_client_color_space;
    return OVRP_SUCCESS;
}

// ovrp_GetTimeInSeconds(double* out) — real 0x172ba0, out in x0, `str d0`,
// -1001 on NULL. The same clock the predicted display time comes off, because a
// guest that compares the two is measuring latency and two clocks would make
// that number meaningless.
static uint64_t klovrp_GetTimeInSeconds(double *out) {
    ovrp_hit("ovrp_GetTimeInSeconds");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = kl_ovrp_predicted_display_time();
    return OVRP_SUCCESS;
}

// --- The tracking-transform family ---------------------------------------
// (The four tracking-transform getters and GetDominantHand are answered with
// the tracking origin, above.)

// --- Which controllers exist ---------------------------------------------
//
// ovrp_GetConnectedControllers2 / ovrp_GetActiveController2 (real 0x170780 /
// 0x170710: out-pointer in x0, one `str w8`, -1001 on NULL). These MUST agree
// with fill_controller_state, which serves both hands for any mask that names
// them — a guest told "no controllers connected" here will never call
// ovrp_GetControllerState at all, and the input surface goes quiet with nothing
// in any log to say why.
#define OVRP_CTRL_TOUCH (OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH)
static uint64_t klovrp_GetConnectedControllers2(int *out) {
    ovrp_hit("ovrp_GetConnectedControllers2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (int)OVRP_CTRL_TOUCH;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetActiveController2(int *out) {
    ovrp_hit("ovrp_GetActiveController2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (int)OVRP_CTRL_TOUCH;
    return OVRP_SUCCESS;
}
// ovrpHandedness: Unsupported 0, LeftHanded 1, RightHanded 2. Real 0x16eb80
// takes the out-pointer in x0 and -1002s when the plugin is down. Right-handed
// is the Quest default and the one a title's "dominant hand" UI assumes.
// ovrp_GetCurrentInteractionProfile(ovrpHand, ovrpInteractionProfile* x1) —
// real 0x175720. None 0, Touch 1, TouchPro 2. Touch, because that is what the
// two entry points above say is connected and what fill_controller_state
// populates; None here would contradict both.
static uint64_t klovrp_GetCurrentInteractionProfile(int hand, int *out) {
    ovrp_hit("ovrp_GetCurrentInteractionProfile");
    (void)hand;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1;
    return OVRP_SUCCESS;
}

// --- Display cadence ------------------------------------------------------
// ovrp_GetSystemVSyncCount2(int* x0) and ovrp_GetAppFramerate2(float* x0), both
// -1001 on NULL. One vsync per frame, and the frame rate is the compositor's
// measured one — the same source ovrp_GetSystemDisplayFrequency2 answers from,
// which a guest may well compare it against.
static uint64_t klovrp_GetSystemVSyncCount2(int *out) {
    ovrp_hit("ovrp_GetSystemVSyncCount2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetAppFramerate2(float *out) {
    ovrp_hit("ovrp_GetAppFramerate2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = kl_ovrp_display_frequency();
    return OVRP_SUCCESS;
}

// ovrp_GetNodePoseStateRaw(ovrpStep step, int frameIndex, ovrpNode node,
//                          ovrpPoseStatef* out) — real 0x170450: w0/w1/w2 and
// the out-pointer in x3 (`mov x19, x3`, -1001 on NULL). The frame index is the
// only thing that distinguishes it from ovrp_GetNodePoseState3, and this
// runtime publishes one pose per guest frame (see the note above
// klovrp_GetNodePoseState_impl), so the RAW pose and the composed pose are the
// same measurement — answering it from anywhere else would be a second head
// position that drifts from the one the frame was rendered with.
static uint64_t klovrp_GetNodePoseStateRaw(int step, int frame_index, int node,
                                           void *out) {
    ovrp_hit("ovrp_GetNodePoseStateRaw");
    (void)frame_index;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    return klovrp_GetNodePoseState_impl(step, node, out);
}

// ovrp_GetAudioOutDeviceId2 / ovrp_GetAudioInDeviceId2 (real 0x16e880 /
// 0x16e920, out-pointer in x0). There is exactly one audio device on this host
// and the OpenSL/CoreAudio bridge already owns it, so 0 — "the default device" —
// is the only id that is true. A refusal here sends UE4 hunting for a device
// list that does not exist.
static uint64_t klovrp_GetAudioOutDeviceId2(int *out) {
    ovrp_hit("ovrp_GetAudioOutDeviceId2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetAudioInDeviceId2(int *out) {
    ovrp_hit("ovrp_GetAudioInDeviceId2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
// ovrp_GetSystemRegion2(ovrpSystemRegion* x0). Unspecified (0) — this is a
// Quest retail/China SKU discriminator and neither is what we are.
static uint64_t klovrp_GetSystemRegion2(int *out) {
    ovrp_hit("ovrp_GetSystemRegion2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
// ovrp_GetAppHasSystemOverlayPresent(ovrpBool* x0) — an out-param bool, so it
// cannot go in g_ovrp_bool_no: that list is for entry points that RETURN the
// bool. Nothing overlays this app; a spurious yes is a guest that pauses itself.
static uint64_t klovrp_GetAppHasSystemOverlayPresent(int *out) {
    ovrp_hit("ovrp_GetAppHasSystemOverlayPresent");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// ovrp_IsLayerShapeSupported(ovrpShape shape, ovrpBool* out) — real 0x175860,
// shape in w0 and the out-pointer in x1 (it does NOT null-check; we do).
//
// This is the predicate in front of the whole g_ovrp_layer_no family, and it
// has to give the same answer: EyeFov yes, everything else no. UE4.27 asks it
// before creating a stereo layer, so answering yes to a quad here would produce
// a setup call that klovrp_refuse then denies — the guest asking twice and being
// told different things.
static uint64_t klovrp_IsLayerShapeSupported(int shape, int *out) {
    ovrp_hit("ovrp_IsLayerShapeSupported");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (shape == KLOVRP_SHAPE_EYEFOV) ? 1 : 0;
    return OVRP_SUCCESS;
}

// --- Application SpaceWarp ------------------------------------------------
// Real 0x172bf0/0x172c20/0x172c50/0x172cb0, each with the out-pointer in x0.
// There is no ASW on this compositor: motion vectors are never submitted
// (klovrp_fill_eye_desc hands out no motion-vector texture and
// ovrp_GetLayerTextureSpaceWarp is refused), so "not adaptive, not requesting
// data, unit scales" is the whole truth and it agrees with the layer family.
static uint64_t klovrp_GetASWVelocityScale(float *out) {
    ovrp_hit("ovrp_GetASWVelocityScale");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1.0f;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetASWDepthScale(float *out) {
    ovrp_hit("ovrp_GetASWDepthScale");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1.0f;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetASWAdaptiveMode(int *out) {
    ovrp_hit("ovrp_GetASWAdaptiveMode");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_IsRequestingASWData(int *out) {
    ovrp_hit("ovrp_IsRequestingASWData");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// ovrp_SetHeadPoseModifier(const ovrpQuatf*, const ovrpVector3f*) and its Get
// (real 0x1705b0 / 0x170600: both pointers checked, -1001 if either is NULL).
// Recorded and echoed rather than answered from a constant, for the same reason
// as the performance levels above — a guest that sets a modifier and reads back
// identity would conclude the plugin refused it. Nothing here APPLIES the
// modifier; UE4.27 does not set one, and if a title starts to, this pair is
// where the log will show it.
static float g_head_pose_mod[7] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
static uint64_t klovrp_SetHeadPoseModifier(const float *rot, const float *pos) {
    ovrp_hit("ovrp_SetHeadPoseModifier");
    if (!rot || !pos) return OVRP_FAIL_INVALID_PARAM;
    for (int i = 0; i < 4; i++) g_head_pose_mod[i] = rot[i];
    for (int i = 0; i < 3; i++) g_head_pose_mod[4 + i] = pos[i];
    static int said;
    if (!said++)
        fprintf(stderr, "  [ovrp] SetHeadPoseModifier: recorded, NOT applied — "
                        "rot %.3f %.3f %.3f %.3f pos %.3f %.3f %.3f\n",
                (double)rot[0], (double)rot[1], (double)rot[2], (double)rot[3],
                (double)pos[0], (double)pos[1], (double)pos[2]);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetHeadPoseModifier(float *rot, float *pos) {
    ovrp_hit("ovrp_GetHeadPoseModifier");
    if (!rot || !pos) return OVRP_FAIL_INVALID_PARAM;
    for (int i = 0; i < 4; i++) rot[i] = g_head_pose_mod[i];
    for (int i = 0; i < 3; i++) pos[i] = g_head_pose_mod[4 + i];
    return OVRP_SUCCESS;
}

// --- The Guardian 2-forms -------------------------------------------------
// The v1 forms already answer "no boundary here" in g_ovrp_bool_no. These two
// carry the answer in an out-param instead of the return, so they are written
// out explicitly: an out-param left untouched is a caller reading its own
// uninitialised stack and calling it a boundary.
// Real 0x171a60 / 0x171b00, out-pointer in x1 / x0.
static uint64_t klovrp_GetBoundaryDimensions2(int type, float *out) {
    ovrp_hit("ovrp_GetBoundaryDimensions2");
    (void)type;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    out[0] = out[1] = out[2] = 0.0f;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetBoundaryVisible2(int *out) {
    ovrp_hit("ovrp_GetBoundaryVisible2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// The refusal class for entry points that answer through an OUT-PARAM and have
// nothing to put in it. -1004 is ovrpFailure_Unsupported, and the real
// library's own failure arms leave the out-param untouched exactly like this
// (see the `tbnz w0, #0x1f` skip over the store in every forwarder above), so a
// caller that checks the result — which every one of these has to, because the
// real plugin fails them on a Quest without the feature — sees the same thing it
// would see there.
//
// This is deliberately NOT g_ovrp_result_ok: answering "success" and leaving the
// out-param alone is how a guest ends up reading its own stack as a pose, a
// space handle or a camera intrinsic.
static uint64_t klovrp_unsupported(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) {
        g_ovrp[s].calls++;
        if (g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] %s -> ovrpFailure_Unsupported (no such "
                            "capability on this host; the out-param is left "
                            "untouched, as the real plugin does)\n", name);
    }
    return (uint64_t)(int64_t)OVRP_FAIL_UNSUPPORTED;
}

// Every one of these was RESOLVED by this guest's Oculus plugin wrapper at load
// (UE4 resolves its whole function table up front) and is therefore one
// Blueprint node away from being a fatal. None of them is on the boot-to-menu
// path we expect, but "expect" is what cost two launches — and the answer is
// the same "no" the capability predicates already give, so listing them costs
// nothing and removes a whole class of one-name-per-launch bring-ups.
static const char *const g_ovrp_result_unsupported[] = {
    // (Every name answered elsewhere is deliberately ABSENT from this list —
    // it is consulted before g_ovrp_result_ok / bool_yes / bool_no, so a
    // duplicate entry here would shadow that answer.)
    // Spatial anchors and the Scene API. ovrp_GetSpaceComponentStatus and its
    // family are gated behind a query that never returns a space here.
    "ovrp_LocateSpace", "ovrp_CreateSpatialAnchor", "ovrp_DestroySpace",
    "ovrp_SetSpaceComponentStatus", "ovrp_GetSpaceComponentStatus",
    "ovrp_EnumerateSpaceSupportedComponents", "ovrp_QuerySpaces",
    "ovrp_RetrieveSpaceQueryResults", "ovrp_SaveSpace", "ovrp_EraseSpace",
    "ovrp_GetSpaceUuid", "ovrp_SaveSpaceList", "ovrp_ShareSpaces",
    "ovrp_CreateSpaceUser", "ovrp_DestroySpaceUser", "ovrp_GetSpaceContainer",
    "ovrp_GetSpaceBoundingBox2D", "ovrp_GetSpaceBoundingBox3D",
    "ovrp_GetSpaceSemanticLabels", "ovrp_GetSpaceRoomLayout",
    "ovrp_GetSpaceBoundary2D", "ovrp_RequestSceneCapture",
    // Insight passthrough. No camera feed exists to composite. The rest of the
    // family is answered BY NAME in g_ovrp_impl (-1002 / bare-bool false /
    // refuse, the real plugin's uninitialised shapes), which
    // is consulted first — so those names were taken off this list.
    "ovrp_CreateInsightTriangleMesh", "ovrp_DestroyInsightTriangleMesh",
    // Mixed Reality Capture and the external camera. ovrp_InitializeMixedReality
    // is already refused above and ovrp_GetMixedRealityInitialized already says
    // no; these are the rest of the family, so a fork that skips the predicate
    // lands on a refusal instead of an abort.
    "ovrp_ShutdownMixedReality",
    // NOTE: six Media names are deliberately ABSENT from this list —
    // ovrp_Media_Initialize, ovrp_Media_GetInitialized,
    // ovrp_Media_SetMrcActivationMode, ovrp_Media_SetMrcAudioSampleRate and the
    // MrcInputVideoBufferType pair. They are already answered by the Media
    // facade in g_ovrp_result_ok / g_ovrp_bool_yes that the Unity guests use,
    // and this list is consulted BEFORE those, so repeating them here would
    // silently change five titles' answers to serve one that never calls them.
    // Face tracking. The Supported/Enabled predicates already say no; these
    // are the state reader and the start/stop pair behind them. (The eye-
    // tracking start/stop/gaze trio is answered success in g_ovrp_result_ok,
    // which a title starts per session and reads as "no gaze".)
    "ovrp_GetFaceState", "ovrp_StartFaceTracking", "ovrp_StopFaceTracking",
    "ovrp_GetFoveationEyeTrackedCenter",
    // Hand-tracking latency, behind ovrp_GetHandTrackingEnabled's no.
    "ovrp_GetHandNodePoseStateLatency", "ovrp_SetHandNodePoseStateLatency",
    // Behind ovrp_GetGPUUtilSupported's no.
    "ovrp_GetGPUUtilLevel",
    // Guardian geometry and the boundary tests: there is no boundary to
    // describe or intersect. The dimensions and visibility 2-forms ARE answered
    // (above) because a "no" fits in their out-param; a geometry buffer and a
    // test result do not.
    "ovrp_GetBoundaryGeometry3", "ovrp_TestBoundaryNode2",
    "ovrp_TestBoundaryPoint2",
    // A mirror texture is a desktop-preview feature; there is no second display.
    "ovrp_SetupMirrorTexture2", "ovrp_DestroyMirrorTexture2",
    // No occlusion mesh data exists here — the same answer the v1 form gives in
    // g_ovrp_bool_no, in the shape this one asks for.
    "ovrp_GetLayerOcclusionMesh",
    // Perf timings we do not measure. Reporting zeros would read as a frame
    // that cost nothing, which is a worse lie than "not available".
    "ovrp_GetAppLatencyTimings2",
    // The system UI (store, guardian setup) cannot be shown from here.
    "ovrp_ShowSystemUI2",
};

static const struct { const char *name; void *fn; } g_ovrp_impl[] = {
    // --- The UE4.27 second wave. See the block above this table. ---
    {"ovrp_GetLayerRecommendedResolution",    (void *)klovrp_GetLayerRecommendedResolution},
    {"ovrp_GetEyeLayerRecommendedResolution", (void *)klovrp_GetEyeLayerRecommendedResolution},
    {"ovrp_GetHmdToEyeOffset2",               (void *)klovrp_GetHmdToEyeOffset2},
    {"ovrp_GetEyePixelsPerTanAngleAtCenter2", (void *)klovrp_GetEyePixelsPerTanAngleAtCenter2},
    {"ovrp_SetSuggestedCpuPerformanceLevel",  (void *)klovrp_SetSuggestedCpuPerformanceLevel},
    {"ovrp_GetSuggestedCpuPerformanceLevel",  (void *)klovrp_GetSuggestedCpuPerformanceLevel},
    {"ovrp_SetSuggestedGpuPerformanceLevel",  (void *)klovrp_SetSuggestedGpuPerformanceLevel},
    {"ovrp_GetSuggestedGpuPerformanceLevel",  (void *)klovrp_GetSuggestedGpuPerformanceLevel},
    {"ovrp_SetAppCPUPriority2",               (void *)klovrp_SetAppCPUPriority2},
    {"ovrp_GetAppCPUPriority2",               (void *)klovrp_GetAppCPUPriority2},
    {"ovrp_SetClientColorDesc",               (void *)klovrp_SetClientColorDesc},
    {"ovrp_GetHmdColorDesc",                  (void *)klovrp_GetHmdColorDesc},
    {"ovrp_GetTimeInSeconds",                 (void *)klovrp_GetTimeInSeconds},
    {"ovrp_GetCurrentTrackingTransformPose",  (void *)klovrp_GetCurrentTrackingTransformPose},
    {"ovrp_GetTrackingTransformRawPose",      (void *)klovrp_GetTrackingTransformRawPose},
    {"ovrp_GetTrackingTransformRelativePose", (void *)klovrp_GetTrackingTransformRelativePose},
    {"ovrp_GetTrackingCalibratedOrigin2",     (void *)klovrp_GetTrackingCalibratedOrigin2},
    {"ovrp_GetConnectedControllers2",         (void *)klovrp_GetConnectedControllers2},
    {"ovrp_GetActiveController2",             (void *)klovrp_GetActiveController2},
    {"ovrp_GetDominantHand",                  (void *)klovrp_GetDominantHand},
    {"ovrp_GetCurrentInteractionProfile",     (void *)klovrp_GetCurrentInteractionProfile},
    {"ovrp_GetSystemVSyncCount2",             (void *)klovrp_GetSystemVSyncCount2},
    {"ovrp_GetAppFramerate2",                 (void *)klovrp_GetAppFramerate2},
    {"ovrp_GetNodePoseStateRaw",              (void *)klovrp_GetNodePoseStateRaw},
    {"ovrp_GetAudioOutDeviceId2",             (void *)klovrp_GetAudioOutDeviceId2},
    {"ovrp_GetAudioInDeviceId2",              (void *)klovrp_GetAudioInDeviceId2},
    {"ovrp_GetSystemRegion2",                 (void *)klovrp_GetSystemRegion2},
    {"ovrp_GetAppHasSystemOverlayPresent",    (void *)klovrp_GetAppHasSystemOverlayPresent},
    {"ovrp_IsLayerShapeSupported",            (void *)klovrp_IsLayerShapeSupported},
    {"ovrp_GetASWVelocityScale",              (void *)klovrp_GetASWVelocityScale},
    {"ovrp_GetASWDepthScale",                 (void *)klovrp_GetASWDepthScale},
    {"ovrp_GetASWAdaptiveMode",               (void *)klovrp_GetASWAdaptiveMode},
    {"ovrp_IsRequestingASWData",              (void *)klovrp_IsRequestingASWData},
    {"ovrp_SetHeadPoseModifier",              (void *)klovrp_SetHeadPoseModifier},
    {"ovrp_GetHeadPoseModifier",              (void *)klovrp_GetHeadPoseModifier},
    {"ovrp_GetBoundaryDimensions2",           (void *)klovrp_GetBoundaryDimensions2},
    {"ovrp_GetBoundaryVisible2",              (void *)klovrp_GetBoundaryVisible2},

    {"UnityPluginLoad",   (void *)klovrp_UnityPluginLoad},
    {"UnityPluginUnload", (void *)klovrp_UnityPluginUnload},
    {"UnityGetAudioEffectDefinitions", (void *)klovrp_UnityGetAudioEffectDefinitions},
    {"JNI_OnLoad",        (void *)klovrp_JNI_OnLoad},
    {"ovrp_GetSkeleton2", (void *)klovrp_GetSkeleton2},
    // ...and the rest of the hand-tracking surface, refused as a group. See
    // klovrp_hand_unsupported for why one handler may serve numbered siblings
    // here when it may not anywhere else in this file.
    {"ovrp_GetSkeleton",  (void *)klovrp_hand_unsupported},
    {"ovrp_GetSkeleton3", (void *)klovrp_hand_unsupported},
    {"ovrp_GetHandState",  (void *)klovrp_hand_unsupported},
    {"ovrp_GetHandState2", (void *)klovrp_hand_unsupported},
    {"ovrp_GetControllerParametricProperties", (void *)klovrp_parametric_unsupported},
    {"ovrp_GetMesh",  (void *)klovrp_hand_unsupported},
    // Body tracking, refused as a group — see klovrp_body_unsupported. The
    // numbered siblings are listed individually, as everywhere else here.
    {"ovrp_RequestBodyTrackingFidelity",  (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyTrackingFidelityStatus", (void *)klovrp_body_unsupported},
    {"ovrp_StartBodyTracking",  (void *)klovrp_body_unsupported},
    {"ovrp_StartBodyTracking2", (void *)klovrp_body_unsupported},
    {"ovrp_StopBodyTracking",   (void *)klovrp_body_unsupported},
    {"ovrp_ResetBodyTrackingCalibration",     (void *)klovrp_body_unsupported},
    {"ovrp_SuggestBodyTrackingCalibrationOverride", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyTrackingCalibrationStatus", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState",  (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState2", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState3", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodySkeleton",  (void *)klovrp_body_unsupported},
    {"ovrp_GetBodySkeleton2", (void *)klovrp_body_unsupported},
    {"ovrp_GetInstanceExtensionsVk", (void *)klovrp_GetInstanceExtensionsVk},
    {"ovrp_GetDeviceExtensionsVk",   (void *)klovrp_GetDeviceExtensionsVk},
    // The init state and everything that reads it. See klovrp_GetInitialized.
    {"ovrp_Initialize5",   (void *)klovrp_Initialize5},
    {"ovrp_RecenterTrackingOrigin2", (void *)klovrp_RecenterTrackingOrigin2},
    {"ovrp_GetAudioOutId2",       (void *)klovrp_GetAudioOutId2},
    {"ovrp_GetAudioInId2",        (void *)klovrp_GetAudioInId2},
    {"ovrp_GetDisplayAdapterId2", (void *)klovrp_GetDisplayAdapterId2},
    {"ovrp_Initialize7",   (void *)klovrp_Initialize7},
    {"ovrp_PreInitialize5", (void *)klovrp_PreInitialize5},
    {"ovrp_GetInitialized", (void *)klovrp_GetInitialized},
    {"ovrp_Shutdown",      (void *)klovrp_Shutdown},
    // UE4.26's numbered siblings of the three above, plus the premultiplied-alpha
    // pair. Shapes read out of tetris's own libOVRPlugin.so — see the block
    // above klovrp_PreInitialize4.
    {"ovrp_PreInitialize4", (void *)klovrp_PreInitialize4},
    {"ovrp_Initialize6",    (void *)klovrp_Initialize6},
    {"ovrp_Shutdown2",      (void *)klovrp_Shutdown2},
    {"ovrp_SetEyeFovPremultipliedAlphaMode",
                            (void *)klovrp_SetEyeFovPremultipliedAlphaMode},
    {"ovrp_GetEyeFovPremultipliedAlphaMode",
                            (void *)klovrp_GetEyeFovPremultipliedAlphaMode},
    {"ovrp_GetAppChromaticCorrection", (void *)klovrp_GetAppChromaticCorrection},
    {"ovrp_SetAppChromaticCorrection", (void *)klovrp_SetAppChromaticCorrection},
    // A real body rather than a name in g_ovrp_result_ok, because the two
    // vectors arrive in s0-s7 and a list entry cannot see them — the recorded
    // values are the whole point. See klovrp_SetColorScaleAndOffset.
    {"ovrp_SetColorScaleAndOffset", (void *)klovrp_SetColorScaleAndOffset},
    {"ovrp_SetAppEngineInfo",  (void *)klovrp_SetAppEngineInfo},
    {"ovrp_SetAppEngineInfo2", (void *)klovrp_SetAppEngineInfo2},
    {"ovrp_GetAppShouldQuit2", (void *)klovrp_GetAppShouldQuit2},
    {"ovrp_GetAppShouldRecreateDistortionWindow2",
     (void *)klovrp_GetAppShouldRecreateDistortionWindow2},
    {"ovrp_GetSystemRecommendedMSAALevel", (void *)klovrp_GetSystemRecommendedMSAALevel},
    {"ovrp_GetSystemRecommendedMSAALevel2", (void *)klovrp_GetSystemRecommendedMSAALevel2},
    {"ovrp_GetGPUFrameTime",   (void *)klovrp_GetGPUFrameTime},
    {"ovrp_InitializeMixedReality", (void *)klovrp_InitializeMixedReality},
    {"ovrp_SetControllerVibration2", (void *)klovrp_SetControllerVibration2},
    {"ovrp_SetControllerLocalizedVibration",
     (void *)klovrp_SetControllerLocalizedVibration},
    {"ovrp_GetEnvironmentDepthSupported", (void *)klovrp_GetEnvironmentDepthSupported},
    {"ovrp_GetEnvironmentDepthHandRemovalSupported",
     (void *)klovrp_GetEnvironmentDepthHandRemovalSupported},
    {"ovrp_InitializeEnvironmentDepth", (void *)klovrp_InitializeEnvironmentDepth},
    {"ovrp_DestroyEnvironmentDepth",    (void *)klovrp_DestroyEnvironmentDepth},
    {"ovrp_StartEnvironmentDepth",      (void *)klovrp_StartEnvironmentDepth},
    {"ovrp_StopEnvironmentDepth",       (void *)klovrp_StopEnvironmentDepth},
    {"ovrp_SetEnvironmentDepthHandRemoval",
     (void *)klovrp_SetEnvironmentDepthHandRemoval},
    {"ovrp_GetEnvironmentDepthFrameDesc", (void *)klovrp_GetEnvironmentDepthFrameDesc},
    {"ovrp_GetEnvironmentDepthTextureDesc",
     (void *)klovrp_GetEnvironmentDepthTextureDesc},
    {"ovrp_GetEnvironmentDepthTexture", (void *)klovrp_GetEnvironmentDepthTexture},
    {"ovrp_GetEnvironmentDepthTextureStageCount",
     (void *)klovrp_GetEnvironmentDepthTextureStageCount},
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
    // The capability sweep — see KLOVRP_CAPABILITY_OFF for why these are entries
    // here and not names in g_ovrp_bool_no.
    {"ovrp_GetHandTrackingEnabled",     (void *)klovrp_GetHandTrackingEnabled},
    {"ovrp_GetEyeTrackingSupported",    (void *)klovrp_GetEyeTrackingSupported},
    {"ovrp_GetEyeTrackingEnabled",      (void *)klovrp_GetEyeTrackingEnabled},
    {"ovrp_GetFaceTrackingSupported",   (void *)klovrp_GetFaceTrackingSupported},
    {"ovrp_GetFaceTrackingEnabled",     (void *)klovrp_GetFaceTrackingEnabled},
    {"ovrp_GetBodyTrackingSupported",   (void *)klovrp_GetBodyTrackingSupported},
    {"ovrp_GetBodyTrackingEnabled",     (void *)klovrp_GetBodyTrackingEnabled},
    {"ovrp_GetFullBodyTrackingEnabled", (void *)klovrp_GetFullBodyTrackingEnabled},
    {"ovrp_GetFoveationEyeTracked",     (void *)klovrp_GetFoveationEyeTracked},
    {"ovrp_GetTiledMultiResDynamic",    (void *)klovrp_GetTiledMultiResDynamic},
    {"ovrp_GetLocalDimmingSupported",   (void *)klovrp_GetLocalDimmingSupported},
    {"ovrp_GetSystemHmd3DofModeEnabled",
     (void *)klovrp_GetSystemHmd3DofModeEnabled},
    {"ovrp_GetGPUUtilSupported",        (void *)klovrp_GetGPUUtilSupported},
    {"ovrp_GetSystemMultiViewSupported2", (void *)klovrp_GetSystemMultiViewSupported2},
    {"ovrp_GetSystemMultiViewSupported",  (void *)klovrp_GetSystemMultiViewSupported},
    {"ovrp_GetEyeTextureArraySupported2", (void *)klovrp_GetEyeTextureArraySupported2},
    {"ovrp_GetTrackingPositionSupported2", (void *)klovrp_GetTrackingPositionSupported2},
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
    {"ovrp_GetNodeFrustum", (void *)klovrp_GetNodeFrustum},
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
    {"ovrp_EnqueueSetupLayer2", (void *)klovrp_EnqueueSetupLayer2},
    {"ovrp_EnqueueSubmitLayer2", (void *)klovrp_EnqueueSubmitLayer2},
    {"ovrp_SetOverlayQuad3", (void *)klovrp_SetOverlayQuad3},
    {"ovrp_DestroyLayer", (void *)klovrp_DestroyLayer},
    {"ovrp_EnqueueDestroyLayer", (void *)klovrp_EnqueueDestroyLayer},
    {"ovrp_GetLayerTextureStageCount", (void *)klovrp_GetLayerTextureStageCount},
    {"ovrp_GetLayerTexture2", (void *)klovrp_GetLayerTexture2},
    {"ovrp_GetLayerTexturePtr", (void *)klovrp_GetLayerTexturePtr},
    {"ovrp_GetLayerAndroidSurfaceObject", (void *)klovrp_GetLayerAndroidSurfaceObject},
    {"ovrp_GetLayerTextureFoveation", (void *)klovrp_GetLayerTextureFoveation},
    {"ovrp_GetLayerTextureSpaceWarp", (void *)klovrp_GetLayerTextureSpaceWarp},
    {"ovrp_GetViewportStencil", (void *)klovrp_GetViewportStencil},
    {"ovrp_EndFrame4", (void *)klovrp_EndFrame4},
    {"ovrp_Update3", (void *)klovrp_Update3},
    {"ovrp_GetUserIPD2", (void *)klovrp_GetUserIPD2},
    {"ovrp_GetUserEyeHeight2", (void *)klovrp_GetUserEyeHeight2},
    {"ovrp_GetUserIPD", (void *)klovrp_GetUserIPD},
    {"ovrp_GetUserEyeHeight", (void *)klovrp_GetUserEyeHeight},
    {"ovrp_GetAppCpuStartToGpuEndTime2", (void *)klovrp_GetAppCpuStartToGpuEndTime2},
    {"ovrp_GetAdaptiveGpuPerformanceScale2", (void *)klovrp_GetAdaptiveGpuPerformanceScale2},
    {"ovrp_IsPerfMetricsSupported", (void *)klovrp_IsPerfMetricsSupported},
    {"ovrp_IsInsightPassthroughSupported", (void *)klovrp_IsInsightPassthroughSupported},
    {"ovrp_InitializeInsightPassthrough", (void *)klovrp_InitializeInsightPassthrough},
    {"ovrp_ShutdownInsightPassthrough", (void *)klovrp_ShutdownInsightPassthrough},
    {"ovrp_GetInsightPassthroughInitialized",
     (void *)klovrp_GetInsightPassthroughInitialized},
    {"ovrp_GetInsightPassthroughInitializationState",
     (void *)klovrp_GetInsightPassthroughInitializationState},
    {"ovrp_GetPassthroughCapabilityFlags", (void *)klovrp_GetPassthroughCapabilityFlags},
    {"ovrp_GetPassthroughCapabilities", (void *)klovrp_GetPassthroughCapabilities},
    {"ovrp_GetPassthroughPreferences", (void *)klovrp_GetPassthroughPreferences},
    {"ovrp_AddInsightPassthroughSurfaceGeometry",
     (void *)klovrp_AddInsightPassthroughSurfaceGeometry},
    {"ovrp_DestroyInsightPassthroughGeometryInstance",
     (void *)klovrp_DestroyInsightPassthroughGeometryInstance},
    {"ovrp_UpdateInsightPassthroughGeometryTransform",
     (void *)klovrp_UpdateInsightPassthroughGeometryTransform},
    {"ovrp_SetInsightPassthroughStyle",  (void *)klovrp_SetInsightPassthroughStyle},
    {"ovrp_SetInsightPassthroughStyle2", (void *)klovrp_SetInsightPassthroughStyle2},
    {"ovrp_SetInsightPassthroughKeyboardHandsIntensity",
     (void *)klovrp_SetInsightPassthroughKeyboardHandsIntensity},
    {"ovrp_CreatePassthroughColorLut",  (void *)klovrp_CreatePassthroughColorLut},
    {"ovrp_DestroyPassthroughColorLut", (void *)klovrp_DestroyPassthroughColorLut},
    {"ovrp_UpdatePassthroughColorLut",  (void *)klovrp_UpdatePassthroughColorLut},
    {"ovrp_QplMarkerStart",            (void *)klovrp_QplMarkerStart},
    {"ovrp_QplMarkerEnd",              (void *)klovrp_QplMarkerEnd},
    {"ovrp_QplMarkerPoint",            (void *)klovrp_QplMarkerPoint},
    {"ovrp_QplMarkerPointCached",      (void *)klovrp_QplMarkerPointCached},
    {"ovrp_QplMarkerPointData",        (void *)klovrp_QplMarkerPointData},
    {"ovrp_QplMarkerAnnotation",       (void *)klovrp_QplMarkerAnnotation},
    {"ovrp_QplMarkerAnnotationVariant",(void *)klovrp_QplMarkerAnnotationVariant},
    {"ovrp_QplCreateMarkerHandle",     (void *)klovrp_QplCreateMarkerHandle},
    {"ovrp_QplDestroyMarkerHandle",    (void *)klovrp_QplDestroyMarkerHandle},
    {"ovrp_QplSetConsent",             (void *)klovrp_QplSetConsent},
    {"ovrp_GetPerfMetricsFloat", (void *)klovrp_GetPerfMetricsFloat},
    {"ovrp_GetPerfMetricsInt", (void *)klovrp_GetPerfMetricsInt},
    {"ovrp_GetAppPerfStats2", (void *)klovrp_GetAppPerfStats2},
    {"ovrp_SetSystemCpuLevel", (void *)klovrp_SetSystemCpuLevel},
    {"ovrp_SetSystemCpuLevel2", (void *)klovrp_SetSystemCpuLevel2},
    {"ovrp_GetSystemCpuLevel2", (void *)klovrp_GetSystemCpuLevel2},
    {"ovrp_SetSystemGpuLevel", (void *)klovrp_SetSystemGpuLevel},
    {"ovrp_SetSystemGpuLevel2", (void *)klovrp_SetSystemGpuLevel2},
    {"ovrp_GetSystemGpuLevel2", (void *)klovrp_GetSystemGpuLevel2},
    {"ovrp_GetSystemCpuLevel", (void *)klovrp_GetSystemCpuLevel},
    {"ovrp_GetSystemGpuLevel", (void *)klovrp_GetSystemGpuLevel},
    {"ovrp_CalculateEyeViewportRect", (void *)klovrp_CalculateEyeViewportRect},
    {"ovrp_CalculateEyePreviewRect", (void *)klovrp_CalculateEyePreviewRect},
    {"ovrp_GetAppPerfStats", (void *)klovrp_GetAppPerfStats_entry},
    {"ovrp_GetBoundaryDimensions", (void *)klovrp_GetBoundaryDimensions},
    {"ovrp_GetControllerState2", (void *)klovrp_GetControllerState2_entry},
    {"ovrp_GetControllerState", (void *)klovrp_GetControllerState_entry},
    {"ovrp_GetControllerState4", (void *)klovrp_GetControllerState4},
    {"ovrp_GetControllerState5", (void *)klovrp_GetControllerState5},
    {"ovrp_GetControllerState6", (void *)klovrp_GetControllerState6},
    {"ovrp_GetTrackingOrientationSupported2", (void *)klovrp_GetTrackingOrientationSupported2},
    {"ovrp_GetTrackingPositionEnabled2",      (void *)klovrp_GetTrackingPositionEnabled2},
    {"ovrp_GetTrackingOrientationEnabled2",   (void *)klovrp_GetTrackingOrientationEnabled2},
    {"ovrp_GetTrackingIPDEnabled2",           (void *)klovrp_GetTrackingIPDEnabled2},
    {"ovrp_GetBodyState4", (void *)klovrp_GetBodyState4},
    {"ovrp_GetUserNeckEyeDistance2", (void *)klovrp_GetUserNeckEyeDistance2},
    {"ovrp_GetAppAsymmetricFov", (void *)klovrp_GetAppAsymmetricFov},
    {"ovrp_GetAppHasInputFocus", (void *)klovrp_GetAppHasInputFocus},
    {"ovrp_GetNativeXrApiType", (void *)klovrp_GetNativeXrApiType},
    {"ovrp_GetSystemDisplayAvailableFrequencies", (void *)klovrp_GetSystemDisplayAvailableFrequencies},
    {"ovrp_SetupEyeTexture2", (void *)klovrp_SetupEyeTexture2},
    {"ovrp_DestroyEyeTexture", (void *)klovrp_DestroyEyeTexture},
    {"ovrp_Update2", (void *)klovrp_Update2},
    {"ovrp_GetControllerHapticsDesc", (void *)klovrp_GetControllerHapticsDesc_entry},
    // Haptics out. All three must be real together: the descriptor sizes
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
    // Tearing down a passthrough that never initialised trivially succeeds.
    "ovrp_ShutdownInsightPassthrough",
    // --- UE4.27: void configuration and fire-and-forget calls. ---
    // Telemetry. The real 1.60 ovrp_SendEvent2 is literally `mov w0, wzr; ret`
    // — Meta stripped it — so success-and-drop is not an approximation here, it
    // is what the shipped library does.
    "ovrp_SendEvent", "ovrp_SendEvent2", "ovrp_AddCustomMetadata",
    "ovrp_SetDeveloperMode",
    // Hints with nothing behind them, like the thread-scheduling pair above.
    "ovrp_SetASWAdaptiveMode", "ovrp_SetRemoteHandedness",
    "ovrp_ResetAppPerfStats2",
    // The haptics forms this file does not decode. ovrp_SetControllerHaptics
    // (the v1 buffer form) IS implemented and drives the host; these three are
    // 1.60 additions whose payload layouts are not read off the library yet, so
    // they are ACCEPTED AND DROPPED rather than refused — refusing a haptics
    // call is a guest that retries it every frame. If the end-of-run surface
    // report shows any of them being called, that is the signal to decode it.
    "ovrp_SetControllerHaptics2",
    "ovrp_SetControllerHapticsAmplitudeEnvelope",
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
    // Bring-up, the same settled decision as ovrp_Initialize5: answer SUCCESS
    // and stand behind it, rather than reporting a failure Unity would be right
    // to believe.
    // ovrp_PreInitialize3 is the numbered sibling 1.40's libOculusXRPlugin
    // dlsyms (its own real implementation is a relay to ovrp_PreInitialize5 that
    // zeroes the three input args and returns ovrpResult 0 on first call, and on
    // THIS host this is always the first call). Numbered variants are listed
    // rather than matched by prefix: a numbered variant can return something
    // DIFFERENT under a familiar name.
    // ovrp_Initialize5 and ovrp_Initialize7 answer the same success but need
    // real implementations, because the answer has to be RECORDED — see
    // klovrp_GetInitialized.
    // Every PreInitialize arity answers the same success no-op — the numbered
    // variants are just which OVRPlugin build the guest was linked against
    // (TWD2 dlsyms PreInitialize4, others 3/5). Listed by name rather than by
    // prefix so a variant that ever returns something different is not silently
    // swept in here.
    "ovrp_PreInitialize", "ovrp_PreInitialize3", "ovrp_PreInitialize4",
    "ovrp_PreInitialize5",
    // Quest Performance Logging (QPL) markers — Meta telemetry. wanderer/UE5
    // instruments frames with these; nothing logs here, so each is a success
    // no-op (returns ignored by the guest).
    "ovrp_QplMarkerStart", "ovrp_QplMarkerEnd", "ovrp_QplMarkerPoint",
    "ovrp_QplMarkerPointCached", "ovrp_QplMarkerAnnotation",
    "ovrp_QplCreateMarkerHandle", "ovrp_QplDestroyMarkerHandle",
    "ovrp_QplSetConsent", "ovrp_QplMarkerAnnotationWithType",
    // Unified telemetry-consent flow (Meta's data-collection opt-in). UE5's
    // Meta XR plugin queries ovrp_GetUnifiedConsent at startup, before any
    // rendering — olar aborted here on the unimplemented trampoline (signal 6).
    // There is no telemetry pipe on this host (the Qpl* markers above are
    // no-ops), so the whole family answers ovrpResult success and touches no
    // out-param: for GetUnifiedConsent / the ShouldShow* predicates that means
    // the guest's pre-zeroed consent/should-show local stays false ("no consent
    // recorded, nothing to show"), which suppresses the consent dialog rather
    // than driving it into the buffer-filling GetConsent*Text getters. The
    // Save*/Set* half are pure setters — success is unambiguously safe. Return 0
    // and read nothing is safe under either ABI (ovrpResult+outparam or a bare
    // ovrpBool), so no argument is dereferenced.
    "ovrp_GetUnifiedConsent", "ovrp_SaveUnifiedConsent",
    "ovrp_SaveUnifiedConsentWithOlderVersion", "ovrp_SetDeveloperTelemetryConsent",
    "ovrp_ShouldShowTelemetryConsentWindow", "ovrp_ShouldShowTelemetryNotification",
    "ovrp_SetNotificationShown",
    // ...and the consent-TEXT getters. olar calls ovrp_GetConsentSettingsChangeText
    // even though the ShouldShow* predicates answered "nothing to show" — it aborted
    // on the unimplemented trampoline (signal 6). Answering success and touching no
    // out-param leaves the guest's text buffer as it found it (empty on the common
    // pre-zeroed path); there is no consent dialog to populate on this host anyway.
    // Names cover the Meta XR consent-text family; unused ones are harmless.
    "ovrp_GetConsentSettingsChangeText", "ovrp_GetConsentTitleText",
    "ovrp_GetConsentMarkdownText", "ovrp_GetConsentNotificationText",
    "ovrp_GetConsentText", "ovrp_GetConsentTitle",
    "ovrp_GetConsentNotificationMarkdownText",
    // Telemetry event sink (olar) — there is no telemetry pipe here; accept and drop.
    "ovrp_SendEvent2", "ovrp_SendEvent",
    // Per-eye buffer sharpening hint (wanderer, UE5). Advisory image post; nothing
    // to configure on the visionOS compositor path, so accept the set as a no-op.
    "ovrp_SetEyeBufferSharpenType",
    // wanderer polls the active interaction profile per frame. Success + a pre-zeroed
    // out reads as "profile 0"; controllers still flow through GetControllerState6, so
    // this only gates a binding-string lookup the guest does not need on this path.
    "ovrp_GetCurrentInteractionProfile",
    // Environment depth (Quest 3 passthrough occlusion) — wanderer probes the frame
    // descriptor. No depth pipe on visionOS; success + a zeroed desc reads as an
    // empty/absent depth frame, which the guest treats as "no occlusion this frame".
    "ovrp_GetEnvironmentDepthFrameDesc",
    // Controller haptics — wanderer fires it the frame after it starts submitting
    // eye layers. No haptics sink on visionOS controllers here; accept and drop.
    "ovrp_SetControllerLocalizedVibration",
    // Eye tracking (Quest Pro) — wanderer starts/stops it per session. No eye
    // tracker on this path; accept the start/stop, and gaze reads stay zeroed.
    "ovrp_StartEyeTracking", "ovrp_StopEyeTracking",
    // Per-frame eye-gaze poll (wanderer, right after StartEyeTracking). Success +
    // a pre-zeroed EyeGazesState reads as "no gaze this frame", which the guest
    // treats as gaze unavailable rather than a fault.
    "ovrp_GetEyeGazesState",
    // wanderer (UE5) polls ovrp_GetUserNeckEyeDistance2 from its per-frame path
    // (right before EndFrame4) and aborted on the unimplemented trampoline
    // (signal 6). It is the OVR neck-model arm length; answering success and
    // touching no out-param leaves the guest's pre-zeroed local at 0, i.e. NO
    // neck-model offset — the head pose we already supply is used directly, which
    // is correct here (we hand a real tracked head pose, not a neck-pivoted one).
    // The paired setter is a pure sink. ABI-safe under result+outparam or bare
    // ovrpBool, same as the consent family above.
    "ovrp_GetUserNeckEyeDistance2", "ovrp_SetUserNeckEyeDistance2",
    // Configuration the guest sets and never reads back.
    "ovrp_SetAppAsymmetricFov",
    // Performance-level hints (CPU/GPU). Advisory on real hardware and there is
    // nothing to set here; answer success so the guest's tuning path completes.
    "ovrp_SetSuggestedCpuPerformanceLevel", "ovrp_SetSuggestedGpuPerformanceLevel",
    "ovrp_GetSuggestedCpuPerformanceLevel", "ovrp_GetSuggestedGpuPerformanceLevel",
    // Eye-tracked foveation hint. No eye-tracked foveation here; accept the set
    // so the guest's foveation setup proceeds (a "supported" query left unfilled
    // reads as its pre-zeroed false, i.e. not supported).
    "ovrp_SetFoveationEyeTracked", "ovrp_GetFoveationEyeTracked",
    // HDR/local-dimming panel control; Vision Pro has no such knob, so
    // acknowledge the set as a successful no-op (wrath2 / UE4 VR renderer).
    "ovrp_SetLocalDimming", "ovrp_GetLocalDimming",
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
    // Same class from the other direction: the guest's suggested level, a hint
    // with nothing on this host acting on it. Deadpool VR's UE4 pushes it during
    // InitializeSession beside the CPU/GPU levels above.
    // The rest of UE4.27's InitializeSession configuration push, same recorded-
    // state class as the setters above: eye-tracked foveation (we answer no eye
    // tracking everywhere else), local dimming, sharpen/color hints, and the
    // tracking toggles. Nothing on this host acts on any of them, and a setter
    // that refuses is a different statement from one that records — see the
    // SetTiledMultiResLevel note above.
    "ovrp_SetFoveationEyeTracked", "ovrp_UpdateFoveation",
    "ovrp_SetLocalDimming", "ovrp_SetEyeBufferSharpenType",
    "ovrp_SetTrackingPositionEnabled2", "ovrp_SetTrackingOrientationEnabled2",
    "ovrp_SetTrackingIPDEnabled2", "ovrp_SetNodePositionTracked2",
    "ovrp_SetBoundaryVisible2", "ovrp_SetInhibitSystemUX2",
    "ovrp_SetSystemVSyncCount2", "ovrp_SetUserEyeHeight2", "ovrp_SetUserIPD2",
    "ovrp_SetUserNeckEyeDistance2", "ovrp_SetTrackingCalibratedOrigin2",
    "ovrp_RecenterTrackingOrigin2",
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
    // Audio device ids — PC-legacy queries; NULL until the guest proves it
    // dereferences the answer. The ...2 forms are scalar int returns (the real
    // 0x16db20 NULL-checks its first arg and clamps a negative getter result
    // to 0), so answering 0 is exactly what they do when no Android audio
    // device exists — and our output is CoreAudio, not an Android device.
    "ovrp_GetAudioOutId", "ovrp_GetAudioInId", "ovrp_GetDisplayAdapterId",
    // (the ...2 forms moved to real implementations: ieytd2's libOculusXRPlugin
    // proved the guess above wrong — OculusSystem::Initialize treats them as
    // `ovrpResult f(void **out)`, then COPIES 16 BYTES from the returned
    // pointer. Success-without-write left the zeroed stack slot as the pointer
    // and the copy read address 0: its boot crash. See klovrp_GetAudioOutId2.)
    // Managed-side Media facade init + MRC configuration; ovrpResult/void.
    "ovrp_Media_Initialize", "ovrp_Media_SetMrcAudioSampleRate",
    "ovrp_Media_SetMrcInputVideoBufferType", "ovrp_Media_GetMrcInputVideoBufferType",
    "ovrp_Media_SetMrcActivationMode",
    // MRC's Vulkan queue-index handshake, pushed by OVRManager during init even
    // when nothing is recording and the guest is on GLES (AC Nexus does this).
    // ovrpResult/void like the rest of the Media facade: record and succeed. The
    // encode entries stay off this list until a run proves one is actually
    // called, the way every other entry here earned its place.
    "ovrp_Media_SetAvailableQueueIndexVulkan",
    // The MRC frame-format setters OVRManager pushes right after, in the same
    // init block: image-flipped and inverse-alpha are always set as a pair.
    // ovrpResult/void, record-and-succeed like the rest of the facade - MRC is
    // a casting feature with no camera behind it here.
    "ovrp_Media_SetMrcFrameImageFlipped", "ovrp_Media_SetMrcFrameInverseAlpha",
    // The display-object / distortion-window lifecycle, ovrpResult and not
    // ovrpBool. 1.40 READS the value: `OculusDisplayProvider::
    // CreateMobileDisplayObjects` does `cbnz w0 -> "Failed Oculus context
    // setup: %d"` on the result of ovrp_SetupDisplayObjects2 and returns
    // failure, `GfxThread_Start` bails on that, and Unity stops the display
    // subsystem — a GfxThread_Start immediately followed by GfxThread_Stop,
    // nothing in between and no error anywhere. libunity's legacy VRDevice
    // ignores the return, which is not the same as not reading it.
    //
    // The convention is not a guess: the real libOVRPlugin.so in this APK ends
    // every one of these with `cmp w0, #0 / csel w0, w0, wzr, lt` and returns
    // -1001/-1002 for bad-parameter / not-initialized, i.e. plain ovrpResult
    // where success is 0 and only negatives are failures. Read it there rather
    // than inferring it from what a caller does with it — a caller that ignores
    // the value cannot tell you which value means yes.
    //
    // Un-suffixed and numbered forms are both listed rather than matched by
    // prefix: a numbered suffix marks an ABI revision, and a variant can return
    // something different under a familiar name.
    "ovrp_SetupDistortionWindow", "ovrp_SetupDistortionWindow3",
    "ovrp_SetupDisplayObjects", "ovrp_SetupDisplayObjects2",
    "ovrp_DestroyDistortionWindow", "ovrp_DestroyDistortionWindow2",
    // A literal stub in the real 1.59 plugin (0x29a40: `mov w0, wzr; ret`) —
    // the overlay-quad path is vestigial by this version, managed OVROverlay
    // uses the Enqueue* family instead. 0 is the real answer, not an invention.
    "ovrp_SetOverlayQuad3",
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
    // Passthrough never initialised (see g_ovrp_result_unsupported).
    "ovrp_GetInsightPassthroughInitialized",
    // Unity asks OVRPlugin which rendering-extension hooks it wants (before/after
    // rendering events, etc.). Our replacement has no render-thread bookkeeping,
    // so "no" is the truthful answer — Unity then never issues the events.
    "UnityRenderingExtQuery",
    // Events that must never fire on a healthy run (0x9bbdf8 acts on
    // recenter; quit/recreate tear things down).
    "ovrp_GetAppShouldRecenter", "ovrp_GetAppShouldQuit",
    "ovrp_GetAppShouldRecreateDistortionWindow",
    // No preview-rect override (return 0 = skip, 0x9bcf9c).
    // ovrp_GetEyeTextureArraySupported / ovrp_GetSystemMultiViewSupported are
    // deliberately NOT in this list: on the Vulkan path the answer can be yes,
    // so they are computed (kl_ovrp_multiview()) and a flat no here would
    // contradict the 2-forms.
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
    // Monoscopic rendering: the real 1.59 pair (0x288d0/0x288fc) is recorded
    // state that starts CLEARED, and we render both eyes for real, so "no" is
    // what is true here as well as what the plugin would say untouched.
    // OVRManager reads it while building the main scene's camera rig — this was
    // fatal, not a no-op, on the first run that ever got that far.
    "ovrp_GetAppMonoscopic",
    // No fixed-foveated/tiled multires rendering in our GL gateway.
    "ovrp_GetTiledMultiResSupported",
    // Mixed Reality Capture — Oculus's external-camera compositing path, which
    // this platform has no equivalent for. "No" is both the truthful answer and
    // the one that keeps Unity out of the rest of the family: the external
    // camera and ovrp_Media_* calls are only reached once this says yes, and
    // the Media facade above is already answered on that assumption.
    // Absent from every list, this was FATAL rather than a no-op (the
    // unimplemented-entry-point abort at the top of this file), so a run ended
    // here with no OVRPlugin shutdown — ovrp_Shutdown is left "resolved but
    // never called" in the M6 report, which is the signature to look for.
    "ovrp_GetMixedRealityInitialized",
    // (UE4.27's capability sweep — eye/face/body/hand tracking, eye-tracked
    // foveation, local dimming, TiledMultiResDynamic, 3DoF mode, GPU util — is
    // NOT here: every one of those names has a real ovrpResult+out body in
    // g_ovrp_impl, which writes the out-param this list could not.)
};

// The compositor-layer family — every one refused, and none of them fatal any
// more. See klovrp_CalculateLayerDesc and klovrp_refuse for why the answer
// is no and why CalculateLayerDesc itself is not in this list.
//
// The whole family is listed, not just the two names this guest is known to
// reach. A layer is a *sequence*: describe, set up, ask its stage count, take
// its textures, submit it every frame, destroy it. Refusing the second step and
// leaving the rest fatal would only move the abort one call to the right, on the
// first title whose fork checks a different return — and the crash it produced
// would name a function that was never the problem.
static const char *const g_ovrp_layer_no[] = {
    // Setup and teardown. EnqueueSetupLayer2 is the one a 1.28+ wrapper actually
    // calls (it carries compositionDepth); the un-suffixed form is the 1.15 ABI
    // and is listed for the same reason as every other numbered pair here.
    "ovrp_EnqueueSetupLayer", "ovrp_EnqueueSetupLayer2", "ovrp_EnqueueDestroyLayer",
    // Per-frame submission.
    "ovrp_EnqueueSubmitLayer", "ovrp_EnqueueSubmitLayer2",
    // The texture accessors. Unreachable once setup is refused — a caller has no
    // layer id to pass — but a fork that ignores the setup result would arrive
    // here with id 0, and "no" is the right answer to that too.
    "ovrp_GetLayerTextureStageCount", "ovrp_GetLayerTexture", "ovrp_GetLayerTexture2",
    "ovrp_GetLayerTexturePtr", "ovrp_GetLayerTextureFoveation",
    // ovrp_GetLayerTextureSpaceWarp is NOT here any more — it is implemented,
    // because its caller treats a refusal as a failure to create the eye
    // textures and tears the whole display provider down. See the
    // implementation for the disassembly that says so.
    "ovrp_GetLayerAndroidSurfaceObject",
    "ovrp_SetupLayerDepth",
    // The eye layer's own id. We do composite the eye layer — but not as a layer
    // the guest can address, and there is no id we could hand over that would
    // mean anything on the calls above.
    "ovrp_GetEyeFovLayerId",
    // The eye-FOV descriptor forms. Same pure computation as CalculateLayerDesc,
    // but for the eye layer, whose description the guest already gets through
    // ovrp_GetEyeTextureSize / GetNodeFrustum2 — answering it twice, in a second
    // shape, is a second place to disagree with ourselves.
    "ovrp_CalculateEyeLayerDesc", "ovrp_CalculateEyeLayerDesc2",
    // The pre-layer overlay ABI (OVRP_0_x). SetOverlayQuad3 is already answered
    // as result-ok above — it is a literal `mov w0, wzr; ret` in the real 1.59
    // plugin, so refusing it would contradict the binary; it stays where it is.
};

static void *klovrp_shared(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_layer_no / sizeof g_ovrp_layer_no[0]; i++)
        if (strcmp(g_ovrp_layer_no[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_refuse);
    for (size_t i = 0; i < sizeof g_ovrp_result_unsupported /
                          sizeof g_ovrp_result_unsupported[0]; i++)
        if (strcmp(g_ovrp_result_unsupported[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_unsupported);
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

// The eye swapchain's own health, split out of kl_ovrp_report so it can also be
// printed DURING a run.
//
// On device the exit report rarely runs — a target may keep the process alive
// when its window closes, and a force quit is SIGKILL — and these four lines
// are the ones a bring-up actually needs. Frames should
// be spread across the stages: all of them on one stage means the guest is not
// cycling and the compositor is reading the texture the guest is writing (see
// KLOVRP_STAGES_DEFAULT).
void kl_ovrp_eye_census(FILE *f) {
    pthread_mutex_lock(&g_frames.mu);
    uint64_t serial = g_frames.serial, guessed = g_frames.guessed;
    uint64_t unobserved = g_frames.unobserved, multi = g_frames.multi;
    uint64_t cross = g_frames.cross_thread, disagree = g_frames.stage_disagree;
    uint64_t named = g_frames.named, named_dis = g_frames.named_disagree;
    uint64_t repeat_drop = g_frames.repeat_drop;
    uint64_t noeye_drop = g_frames.noeye_drop;
    uint64_t filed[KLOVRP_MAX_STAGES];
    memcpy(filed, g_frames.filed, sizeof filed);
    pthread_mutex_unlock(&g_frames.mu);
    if (!serial) return;
    fprintf(f, "  eye swapchain: %d stage(s), frames per stage",
            kl_ovrp_stage_count());
    for (int i = 0; i < kl_ovrp_stage_count(); i++)
        fprintf(f, " %llu", (unsigned long long)filed[i]);
    fprintf(f, " (%llu begun, %llu filed on a guessed stage, "
               "%llu where the guest's frame index disagreed)\n",
            (unsigned long long)serial, (unsigned long long)guessed,
            (unsigned long long)disagree);
    // **WHICH RUNG OF THE LADDER ACTUALLY FILED THE STAGE**, printed because
    // neither number above answers it on a Vulkan guest. `filed on a guessed stage` counts `!binds`, i.e.
    // "kl_glfb saw no GL framebuffer bind" — which is true of EVERY frame a
    // Vulkan guest ever submits, so on such a guest it is a constant, not a
    // finding. `the guest's frame index disagreed` is `stage_disagree`, and its
    // only increment site is guarded by `observed >= 0` — also unreachable
    // without GL. Both read reassuring for a reason that has nothing to do with
    // the stage being right.
    //
    // These two are the reachable ones. `named` is how many frames took the
    // stage from the guest's OWN submit (`texture_stage` at submit+0x04), which
    // is an assertion rather than an inference; `serial - named` is how many
    // fell through to `serial % stages`, which is a guess and is wrong the
    // moment the guest's ring and our counter differ in phase. `named_dis` is
    // how often the guest's answer and the counter disagreed — a nonzero value
    // is the counter being caught wrong, and is exactly the mispairing that
    // shows one frame's pose against another frame's picture.
    fprintf(f, "  eye stage source: %llu named by the guest's submit, %llu fell "
               "through to serial %% %d (%llu of the named ones disagreed with "
               "that counter)\n",
            (unsigned long long)named, (unsigned long long)(serial - named),
            kl_ovrp_stage_count(), (unsigned long long)named_dis);
    // **Frames whose submit named NO eye layer**, which is the producer half of
    // the doubling described in klovrp_submit_viewports. A frame like that used
    // to file a record with a zero render viewport, and a zero rect is read as
    // "the whole texture" — which for a DoubleWide guest shows both halves to
    // both eyes. Printed unconditionally when it has ever happened, because on
    // an Unreal title it happens exactly during the loading screens where the
    // symptom was reported and nowhere else, and a counter that only appears
    // when it is large would not have caught it.
    {
        uint64_t carried = 0, noeye = kl_ovrp_viewport_carry_stats(&carried);
        if (noeye)
            fprintf(f, "  %llu frame(s) submitted NO eye layer (%llu had the last "
                       "render viewport carried into their record; %llu were left "
                       "with the whole-texture fallback)\n",
                    (unsigned long long)noeye, (unsigned long long)carried,
                    (unsigned long long)(noeye - carried));
    }
    // The submit-named stage, and whether it was worth reading. On a Vulkan
    // guest this is the ONLY thing that can say which stage holds a finished
    // picture — the GL observation sees nothing there — so a large
    // disagreement here is a compositor that was reading the stage the guest
    // is drawing into next.
    if (named)
        fprintf(f, "  ...of which %llu took the stage the guest NAMED in its "
                   "submit, %llu of them differing from the frame counter%s\n",
                (unsigned long long)named, (unsigned long long)named_dis,
                named_dis ? "  <-- the counter would have been wrong there" : "");
    // Frames the guest submitted without rotating its stage — a dropped frame,
    // on a guest the GL observation cannot see. Their records are NOT filed, so
    // the compositor keeps showing the previous picture with the pose it was
    // drawn at instead of pairing it with a newer one.
    if (repeat_drop)
        fprintf(f, "  ...and %llu frame(s) named the SAME stage again and were "
                   "dropped as having drawn nothing (KL_OVRP_DROP_REPEAT=0 "
                   "files them anyway)\n",
                (unsigned long long)repeat_drop);
    // Frames that named no eye layer (Unreal's loading screens). Dropped, so
    // they are among `fell through` above only as a count of serials; none of
    // them filed a pose. See klovrp_end_frame_impl.
    if (noeye_drop)
        fprintf(f, "  ...and %llu frame(s) named NO eye layer and were dropped "
                   "(not filed on serial %% %d; KL_OVRP_DROP_NO_EYE=0 files them)\n",
                (unsigned long long)noeye_drop, kl_ovrp_stage_count());
    // What the GL side saw, independently of any of the bookkeeping above. A
    // stage whose draw count stops climbing is a frozen picture, and that is a
    // different bug from a mis-filed pose even though both look like doubling
    // in the headset.
    fprintf(f, "  eye draw targets observed per stage:");
    for (int i = 0; i < kl_ovrp_stage_count(); i++)
        fprintf(f, " %llu", (unsigned long long)kl_glfb_stage_draw_count(i));
    fprintf(f, "\n");
    // The association's own health. All three should be 0; each non-zero one
    // names a different reason the pose filed against a stage may not be the
    // pose its picture was drawn with.
    fprintf(f, "  pose<->picture association: %llu frame(s) drew into no eye "
               "stage, %llu into several, %llu drawn off-thread%s\n",
            (unsigned long long)unobserved, (unsigned long long)multi,
            (unsigned long long)cross,
            (unobserved || multi || cross) ? "" : "  (clean)");
}

void kl_ovrp_report(FILE *f) {
    static int done;
    if (done || !g_novrp) return;
    done = 1;
    unsigned called = 0;
    for (unsigned i = 0; i < g_novrp; i++) if (g_ovrp[i].calls) called++;
    fprintf(f, "\n=== OVRPlugin surface (work list) ===\n");
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
    kl_ovrp_eye_census(f);
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (g_ovrp[i].calls) fprintf(f, "    %-44s x%u\n", g_ovrp[i].name, g_ovrp[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (!g_ovrp[i].calls) fprintf(f, "    %s\n", g_ovrp[i].name);
}

// --- The no-eye-layer drop self-test ----------------------------------------
//
// A Vulkan guest (observation blind) names stages 0 and 1, then submits three
// frames with NO eye layer, then names stage 2. The three must file nothing:
// `last_complete` stays on stage 1 holding frame 2's serial, no stage's filed
// count moves, and the next named frame files normally. Before the fix the
// three landed on `serial % 3`, each over an older picture.
int kl_ovrp_no_eye_drop_selftest(FILE *f) {
    int ok = 1;
    if (!f) f = stderr;
    pthread_mutex_lock(&g_frames.mu);
    int saved_named = g_frames.last_named;
    g_frames.last_named = -1;
    pthread_mutex_unlock(&g_frames.mu);
    int stages = kl_ovrp_stage_count();
    if (stages < 3) {
        fprintf(f, "  (KL_OVRP_STAGES=%d: the case needs 3 stages — skipped)\n", stages);
        return 1;
    }
    int vp[8] = { 0, 0, 100, 100, 0, 0, 100, 100 }, of[2] = { 100, 100 };

    klovrp_begin_frame_impl(1001);
    klovrp_end_frame_impl(1001, vp, of, 0, NULL);
    klovrp_begin_frame_impl(1002);
    klovrp_end_frame_impl(1002, vp, of, 1, NULL);

    pthread_mutex_lock(&g_frames.mu);
    uint64_t serial2 = g_frames.r[1].serial, serial1 = g_frames.r[0].serial;
    uint64_t filed_before[3] = { g_frames.filed[0], g_frames.filed[1], g_frames.filed[2] };
    uint64_t drops_before = g_frames.noeye_drop;
    pthread_mutex_unlock(&g_frames.mu);

    for (int i = 0; i < 3; i++) {
        klovrp_begin_frame_impl(1003 + i);
        klovrp_end_frame_impl(1003 + i, vp, of, -1, NULL);   // carried rect, no stage
    }

    pthread_mutex_lock(&g_frames.mu);
    int lc = g_frames.last_complete;
    int moved = g_frames.filed[0] != filed_before[0] || g_frames.filed[1] != filed_before[1] ||
                g_frames.filed[2] != filed_before[2];
    int kept = g_frames.r[1].serial == serial2 && g_frames.r[0].serial == serial1;
    uint64_t dropped = g_frames.noeye_drop - drops_before;
    pthread_mutex_unlock(&g_frames.mu);
    fprintf(f, "  3 no-eye frames: last_complete %d (want 1), records %s, filed counts %s, "
               "%llu dropped (want 3)\n", lc, kept ? "kept" : "OVERWRITTEN",
            moved ? "MOVED" : "unchanged", (unsigned long long)dropped);
    if (lc != 1 || !kept || moved || dropped != 3) ok = 0;

    klovrp_begin_frame_impl(1006);
    klovrp_end_frame_impl(1006, vp, of, 2, NULL);
    pthread_mutex_lock(&g_frames.mu);
    lc = g_frames.last_complete;
    pthread_mutex_unlock(&g_frames.mu);
    fprintf(f, "  next named frame: last_complete %d (want 2)\n", lc);
    if (lc != 2) ok = 0;

    pthread_mutex_lock(&g_frames.mu);
    if (saved_named >= 0) g_frames.last_named = saved_named;
    pthread_mutex_unlock(&g_frames.mu);
    return ok;
}

// --- The pose-boundary self-test -------------------------------------------
//
// What it asserts is the invariant index keying broke in gameplay: **every head
// pose query inside ONE guest frame cycle returns ONE pose, and the pose
// BeginFrame files for reprojection is that same pose.**
//
// It reproduces the gameplay pattern exactly, because that is the pattern the
// old code failed on and the menu pattern it passed: Unity's render thread
// trails its sim thread, so the cycle's two ovrp_Update2 calls carry DIFFERENT
// frame indices, and the frontend publishes a NEW head pose in between (it runs
// at panel rate on its own thread and does not stop for the guest). Under
// index keying the second Update2 re-froze and the two halves of the frame got
// two heads; under cycle keying the second Update2 reuses the first's.
//
// Node 9 is Head. The quaternion is at f[0..3].
int kl_ovrp_pose_boundary_selftest(FILE *f) {
    int ok = 1;
    if (!f) f = stderr;

    for (int cyc = 1; cyc <= 4; cyc++) {
        const int sim = cyc + 1, ren = cyc;      // the render thread trails one
        float a0 = 0.20f * (float)cyc;           // where the head is at cycle open
        float a1 = a0 + 0.05f;                   // ...and where it has got to mid-frame

        kl_ovrp_set_head_pose(0, 1.6f, 0, 0, sinf(a0 * 0.5f), 0, cosf(a0 * 0.5f));

        klovrp_WaitToBeginFrame(cyc);                    // the cycle opens

        klovrp_Update2(0, sim, 0.0);                     // Physics, sim index
        float p_sim[24] = {0};
        klovrp_GetNodePoseState_impl(0, 9, p_sim);

        // The frontend publishes again, mid-frame. This is the motion the old
        // boundary let into the middle of a guest frame.
        kl_ovrp_set_head_pose(0, 1.6f, 0, 0, sinf(a1 * 0.5f), 0, cosf(a1 * 0.5f));

        klovrp_Update2(KLOVRP_STEP_RENDER, ren, 0.0);    // Render, trailing index
        float p_ren[24] = {0};
        klovrp_GetNodePoseState_impl(KLOVRP_STEP_RENDER, 9, p_ren);

        // ...and one more query after the second step, which is where the
        // majority of the frame's 26 reads actually land.
        float p_late[24] = {0};
        klovrp_GetNodePoseState_impl(0, 9, p_late);

        klovrp_begin_frame_impl(ren);                    // files the anchor

        int split = 0;
        for (int i = 0; i < 4; i++) {
            if (fabsf(p_sim[i] - p_ren[i])  > 1e-6f) split = 1;
            if (fabsf(p_sim[i] - p_late[i]) > 1e-6f) split = 1;
        }
        // The filed anchor, straight out of the pending record BeginFrame just
        // wrote — this is what kl_reproject subtracts.
        pthread_mutex_lock(&g_frames.mu);
        float fq[4] = { g_frames.pending.qx, g_frames.pending.qy,
                        g_frames.pending.qz, g_frames.pending.qw };
        pthread_mutex_unlock(&g_frames.mu);
        int anchor_off = 0;
        for (int i = 0; i < 4; i++)
            if (fabsf(fq[i] - p_sim[i]) > 1e-6f) anchor_off = 1;

        fprintf(f, "  cycle %d (sim index %d, render index %d): "
                   "queries %s, filed anchor %s\n",
                cyc, sim, ren,
                split ? "SPLIT ACROSS TWO POSES" : "one pose",
                anchor_off ? "IS NOT THAT POSE" : "is that pose");
        if (split || anchor_off) ok = 0;
    }

    // The cycles must also be counted: one freeze per WaitToBeginFrame, no
    // more and no fewer. Four cycles above, and the counters are only reset by
    // the 300-frame census, which cannot have fired here.
    fprintf(f, "  freezes %llu over %llu guest frame cycles (must be equal)\n",
            (unsigned long long)g_op_freezes, (unsigned long long)g_op_arms);
    if (g_op_arms == 0 || g_op_freezes != g_op_arms) ok = 0;

    return ok;
}

// --- The floor gate (kl_ovrp.h documents what it asserts) -------------------
//
// The floor answer is two halves of one number, and the failure mode if they ever
// drift is silent: a guest that adds our ovrp_GetUserEyeHeight2 answer to a
// pose we have already raised lands a metre and a half too high, and a guest
// that trusts one and not the other lands in the ground — which is where
// Pinball FX VR's camera was. So the assertion is the IDENTITY between them, not either value.
//
// Composed out of klovrp_GetNodePoseState_impl rather than out of the offset
// helper, because the entry point is what the guest calls and the helper is
// only how it is written today.
int kl_ovrp_floor_selftest(FILE *f) {
    int ok = 1;
    float buf[22];
    const int NODE_HEAD = 9;

    // A head 20 cm off the tracking origin in y, so "the offset was applied"
    // cannot be confused with "the head happened to be at the height we add".
    kl_ovrp_set_head_pose(0.0f, 0.20f, 0.0f, 0, 0, 0, 1);
    kl_ovrp_frame_latch();

    float h = 0.0f;
    void *sym = kl_ovrp_sym("ovrp_GetUserEyeHeight2");
    if (!sym) {
        fprintf(f, "  FAIL: ovrp_GetUserEyeHeight2 resolved to nothing — this is "
                   "the name Pinball FX VR's lobby died on\n");
        return 0;
    }
    if ((int64_t)((uint64_t (*)(float *))sym)(&h) != 0 || h <= 0.5f || h >= 2.5f) {
        fprintf(f, "  FAIL: ovrp_GetUserEyeHeight2 answered %.3f m — a standing "
                   "eye height, not zero and not a tracking origin\n", (double)h);
        ok = 0;
    } else {
        fprintf(f, "  ovrp_GetUserEyeHeight2 = %.3f m\n", (double)h);
    }

    // The NULL refusal, which the real plugin spells -1001 and which a guest
    // that probes for the entry point's existence relies on.
    if ((int64_t)((uint64_t (*)(float *))sym)(NULL) >= 0) {
        fprintf(f, "  FAIL: ovrp_GetUserEyeHeight2(NULL) did not refuse\n");
        ok = 0;
    }

    // EyeLevel: the origin IS the eye, so nothing moves. Asserted first, because
    // an offset that applies unconditionally would pass every test below.
    ((uint64_t (*)(int))kl_ovrp_sym("ovrp_SetTrackingOriginType2"))(0);
    klovrp_GetNodePoseState_impl(-1, NODE_HEAD, buf);
    if (buf[5] < 0.199f || buf[5] > 0.201f) {
        fprintf(f, "  FAIL: EyeLevel head y = %.3f, expected the 0.200 it was "
                   "given — nothing may be added under an eye-level origin\n",
                (double)buf[5]);
        ok = 0;
    } else {
        fprintf(f, "  EyeLevel  head y = %.3f (unchanged)\n", (double)buf[5]);
    }

    // FloorLevel: raised by exactly the height the getter reports, and by
    // exactly it, so the two answers compose to the identity a guest expects.
    ((uint64_t (*)(int))kl_ovrp_sym("ovrp_SetTrackingOriginType2"))(1);
    klovrp_GetNodePoseState_impl(-1, NODE_HEAD, buf);
    float want = klovrp_floor_offset_enabled() ? 0.20f + h : 0.20f;
    if (buf[5] < want - 0.001f || buf[5] > want + 0.001f) {
        fprintf(f, "  FAIL: FloorLevel head y = %.3f, expected %.3f — the raise "
                   "must be the SAME number ovrp_GetUserEyeHeight2 reports\n",
                (double)buf[5], (double)want);
        ok = 0;
    } else {
        fprintf(f, "  FloorLevel head y = %.3f (raised by %.3f, the getter's own "
                   "answer)%s\n", (double)buf[5], (double)(want - 0.20f),
                klovrp_floor_offset_enabled() ? "" : " — KL_OVRP_FLOOR_OFFSET off");
    }

    // ...and the hands come with it. A head on the floor's shelf and hands still
    // in the basement is the same bug, one node over, and less visible.
    klovrp_GetNodePoseState_impl(-1, 3, buf);
    float hand_y = buf[5];
    klovrp_GetNodePoseState_impl(-1, NODE_HEAD, buf);
    if (hand_y - (buf[5] - 0.20f) > 0.20f || hand_y - (buf[5] - 0.20f) < -1.0f) {
        fprintf(f, "  FAIL: the hand did not move with the head (hand y %.3f "
                   "against a head raised to %.3f)\n",
                (double)hand_y, (double)buf[5]);
        ok = 0;
    } else {
        fprintf(f, "  hand y = %.3f, in the same space as the head\n",
                (double)hand_y);
    }

    ((uint64_t (*)(int))kl_ovrp_sym("ovrp_SetTrackingOriginType2"))(0);
    return ok;
}
