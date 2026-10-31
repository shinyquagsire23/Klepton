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
#include <strings.h>
#include <stddef.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
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
#include "kl_openxr.h"
#include "kl_metrics.h"
#include "kl_hands.h"

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
    // KL_OVRP_UNANSWERED=unsupported — an entry point with no answer here says
    // UNSUPPORTED (-1004, out-parameters untouched) instead of ending the game,
    // named once in the log. For a guest that is told a plugin version far above
    // the 1.60.0 the answers in this file were collected against: Demeo, told its
    // own 1.109.0 for the sake of passthrough, can reach some two hundred entry
    // points nothing here has seen called (spaces, scene, virtual keyboard,
    // render models, camera devices, colocation). All of those return ovrpResult,
    // for which "unsupported" is what a headset without the feature says. The
    // old value-returning entry points (ovrp_GetNodePose and the like) are not
    // reached at such a version — the guest's C# prefers their successors — and
    // if one is, the line below names it.
    //   default "unsupported" for x3634830803298285, x1642239225880682,
    //   x2436558143118760 (Synth Riders, told its own 1.115.0) and
    //   x4015163475201433 (LES MILLS BODYCOMBAT, told its own 1.98.0), and every
    //   title under the profile for untested games (kl_untested_profile, which
    //   also tells it its own version); else "abort"
    static int soft = -1;
    if (soft < 0) {
        const char *t = kl_driver_target_name();
        const char *e = kl_env_str("KL_OVRP_UNANSWERED", NULL);
        // ...and Richie's Plank Experience, told its own 1.101.1 for the same reason,
        // and LES MILLS BODYCOMBAT, told its own 1.98.0.
        extern int kl_untested_profile(void);
        soft = e && *e ? !strcmp(e, "unsupported") : (t && (!strcmp(t, "x3634830803298285") ||
                                                             !strcmp(t, "x1642239225880682") ||
                                                             !strcmp(t, "x2436558143118760") ||
                                                             !strcmp(t, "x4015163475201433"))) ||
                                                     kl_untested_profile();
    }
    if (soft) {
        if (s >= 0 && g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] no answer for %s — said UNSUPPORTED (KL_OVRP_UNANSWERED)\n", name);
        return (uint64_t)-1004;    // OVRP_FAIL_UNSUPPORTED, defined further down
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

// The failure half of ovrpResult, by name. Every value here is one the real
// libOVRPlugin.so in this APK returns, counted in its disassembly (-1000 x90,
// -1001 x279, -1002 x296, -1003 x126, -1004 x70, and on down) — transcribed,
// not chosen. Naming them matters because the family below is a chain of
// ovrpResult returns, where answering one of these with the wrong sign reads as
// success.
#define OVRP_FAIL_INVALID_PARAM   (-1001)
#define OVRP_FAIL_NOT_INITIALIZED (-1002)
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
// not exist on this host (body and face tracking, parametric haptics), this is
// the honest answer and it prunes the entire follow-on subtree — the guest sees
// the first call fail and never makes the ones that would follow it.
static uint64_t klovrp_unsupported(const char *name) {
    ovrp_hit(name);
    return OVRP_FAIL_UNSUPPORTED;
}

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
//
// Walkabout (Unity 6000.3, OculusXR 4.x) reads the answer differently: the
// pointer handed back IS the VkPhysicalDevice. Its plugin passes it to Unity
// unread as the XR device, and Unity's SelectPhysicalDevice looks for that exact
// handle among vkEnumeratePhysicalDevices — so the LUID buffer's address matched
// nothing and the guest put up "Unable to initialize the Unity Engine Graphics
// API". KL_OVRP_ADAPTER_PHYS=0/1 forces the A/B either way.
static int klovrp_adapter_is_phys(void) {
    static int on = -1;
    if (on < 0) {
        extern int kl_untested_profile(void);
        extern kl_guest_kind kl_driver_kind(void);
        const char *t = kl_driver_target_name();
        // batman (Unity 2022.3.45) is the second title measured to need it: on
        // the LUID answer it enumerated the one device, read its properties,
        // matched nothing and went straight to "Your device does not match the
        // hardware requirements" and a GLES2 context — with the Adreno identity
        // and the Quest 3 model already in place, so neither of those was the
        // gate. With the handle it creates its device (35 extensions) and renders.
        // Max Mustard (x5269080709783454, Unity 2022.3.16) is the third: same
        // LUID answer, same one-device walk, same fall to GLES2 — whose shaders
        // it does not ship ("shader compiler platform 5"), so every material
        // drew as Unity's pink error shader.
        // Demeo (x3634830803298285, Unity 6000.0.60) is the fourth, and has no
        // GLES to fall to: one device walked, its properties read, then "Unable
        // to initialize the Unity Engine Graphics API" and UnityPlayer.kill()
        // (on device 2026-10-12; with the handle it creates its device on the
        // Mac host and reaches its privacy notice).
        // Arizona Sunshine 2 (x5245041552210029, Unity 2022.3.10) is the fifth:
        // LUID answer, one device walked, "Your device does not match the
        // hardware requirements", a GLES 2.0 context and a black eye with
        // every mesh draw skipped while its audio and game ran on.
        // Ghost Town (x9531494193591052, Unity 2022.3.49) is the sixth: the LUID
        // answer, one device walked, the probe instance destroyed, then a GLES2
        // context and "shader compiler platform 5" — the left eye stayed on
        // that black frame and the right eye was never given a layer.
        // LEGO Bricktales (x6521909757843713, Unity 6000.2, OVRPlugin 1.104)
        // is the seventh, and only one title (acnexusvr) is known to want the
        // LUID. So the profile for untested games takes the handle for every
        // Unity title: a GL one has no Vulkan device, and the LUID answer
        // below still serves it (2026-10-17).
        on = kl_env_on("KL_OVRP_ADAPTER_PHYS",
                       (t && (strcmp(t, "walkabout") == 0 || strcmp(t, "batman") == 0 ||
                              strcmp(t, "x5269080709783454") == 0 ||
                              strcmp(t, "x3634830803298285") == 0 ||
                              strcmp(t, "x5245041552210029") == 0 ||
                              strcmp(t, "x9531494193591052") == 0)) ||
                       (kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY));
    }
    return on;
}
static uint64_t klovrp_GetDisplayAdapterId2(const void **out) {
    ovrp_hit("ovrp_GetDisplayAdapterId2");
    if (klovrp_adapter_is_phys()) {
        void *pd = kl_vulkan_display_phys();
        if (pd) {
            fprintf(stderr, "  [ovrp] display adapter -> VkPhysicalDevice %p\n", pd);
            if (out) *out = pd;
            return OVRP_SUCCESS;
        }
    }
    static uint8_t id[32];
    memset(id, 0, sizeof id);
    if (kl_vulkan_display_luid(id))
        fprintf(stderr, "  [ovrp] display adapter LUID %02x%02x%02x%02x%02x%02x%02x%02x\n",
                id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    if (out) *out = id;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Initialize1(void) {
    klovrp_Initialize("ovrp_Initialize");
    return OVRP_TRUE;
}
static uint64_t klovrp_Initialize2(void) {
    klovrp_Initialize("ovrp_Initialize2");
    return OVRP_TRUE;
}
static uint64_t klovrp_Initialize3(void) { return klovrp_Initialize("ovrp_Initialize3"); }
static uint64_t klovrp_Initialize4(void) { return klovrp_Initialize("ovrp_Initialize4"); }
static uint64_t klovrp_Initialize5(void) { return klovrp_Initialize("ovrp_Initialize5"); }
// The 6 revision sits between the two we already answer; ieytd2's
// libOculusXRPlugin calls it and the unimplemented-entry abort was its whole
// boot. Same answer as 5 and 7 — the revisions differ in arguments we ignore.
static uint64_t klovrp_Initialize6(void) { return klovrp_Initialize("ovrp_Initialize6"); }
static uint64_t klovrp_Initialize7(void) { return klovrp_Initialize("ovrp_Initialize7"); }

// ...and the other end: the guest saying the plugin is down. ovrp_Shutdown is
// the ovrpBool wrapper (`!(result < 0)`), so it answers yes; clearing the flag
// is the load-bearing half, since a GetInitialized still saying yes afterwards
// would send a guest that re-enters InitDevice straight back into the fault
// above. ovrp_Shutdown2, the ovrpResult form, is
// deliberately NOT implemented here: nothing has ever called it, and an
// abort-by-name is how this project finds out that something does.
static uint64_t klovrp_Shutdown(void) {
    ovrp_hit("ovrp_Shutdown");
    g_ovrp_initialized = 0;
    return OVRP_TRUE;
}

// ovrp_Shutdown2, the ovrpResult form. TWD2 (a UE4 DoubleWide title) reaches it
// when its eye layer cannot be served — but a Shutdown call is the guest tearing
// the plugin down, and aborting on it turns an orderly teardown into signal 6 on
// the game thread. Answer OVRP_SUCCESS and clear the flag, exactly as the
// ovrpBool form does. (Whatever drove the guest to shut down is a separate
// question — for TWD2 that is DoubleWide eye layers; see klovrp_GetLayerTexture2.)
static uint64_t klovrp_Shutdown2(void) {
    ovrp_hit("ovrp_Shutdown2");
    g_ovrp_initialized = 0;
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
#define KLOVRP_VERSION_DEFAULT "1.60.0"
// ...per guest, because the number is the guest's: a title built against a newer
// plugin gates whole feature families on it in its OWN C#. Virtual Virtual
// Reality (x2111191602278229, Unity 6 + Meta XR SDK) ships OVRPlugin 1.117.0
// (strings(1) on its libOVRPlugin.so) and runs on Unity's OpenXR plugin, whose
// MetaXRFeature calls OVRPlugin.UnityOpenXR.OnInstanceCreate — and that C# only
// reaches the native entry point at version >= 1.71; below it, it returns false
// without calling anything. Told 1.60.0, VVR logged "[MetaXRFeature]
// OnInstanceCreate returned an error", left OVRPlugin uninitialised
// (userPresent stayed false) and sat on its black startup fade. KL_OVRP_VERSION
// overrides for any guest.
// Beat Saber from 1.45 on (kl_guest_beatsaber_openxr) is the same kind of guest
// and ships OVRPlugin 1.106.0. Told 1.60.0 it logged the same line, never asked
// this plugin for a controller again, and showed its language screen with no
// sabers and no pointer — nothing to press Continue with (on device,
// 2026-10-12). 1.40 ships 1.60.0 and is told 1.60.0 under the same row.
extern int kl_guest_beatsaber_openxr(void);

// The OVRPlugin version a game ships, from its own libOVRPlugin.so: the one
// NUL-delimited "1.<minor>.<patch>" with a minor of 20 or more (the plugin's
// number; the 1.5.x beside it in most builds belongs to something else). The
// same rule as ovrp_guest_version in visionos/targets.py, which reads it when
// the app is built and hands it over as KL_OVRP_GUEST_VERSION — on the device
// the bundle carries the library's translation, not the ELF, so this reader is
// for a Mac host run. Returns 0 and leaves `out` empty unless exactly one
// candidate is found. Exported for tests/t_untested.c.
int kl_ovrp_guest_version_in(const char *path, char *out, size_t cap) {
    if (cap) out[0] = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    char found[24] = "";
    int n = 0;
    char win[24]; size_t wl = 0; int prev_nul = 1;
    for (int c; (c = fgetc(f)) != EOF; ) {
        if (c == 0) {
            if (prev_nul && wl >= 6 && wl < sizeof win) {
                win[wl] = 0;
                unsigned a, b, p; char tail;
                if (sscanf(win, "%u.%u.%u%c", &a, &b, &p, &tail) == 3 && a == 1 && b >= 20 &&
                    b < 1000 && p < 1000 && strspn(win, "0123456789.") == wl) {
                    if (!n) { snprintf(found, sizeof found, "%s", win); n = 1; }
                    else if (strcmp(found, win) != 0) n = 2;
                }
            }
            wl = 0; prev_nul = 1;
            continue;
        }
        if (wl < sizeof win - 1 && ((c >= '0' && c <= '9') || c == '.')) win[wl++] = (char)c;
        else { wl = sizeof win; }       // not a version: skip to the next NUL
    }
    fclose(f);
    if (n != 1) return 0;
    snprintf(out, cap, "%s", found);
    return 1;
}

// What the profile for untested games tells a title as its plugin version:
// KL_OVRP_GUEST_VERSION (from the build), else the library itself on a Mac
// host, else NULL (the default stands).
static const char *klovrp_guest_version(void) {
    static char v[24];
    const char *e = kl_env_str("KL_OVRP_GUEST_VERSION", NULL);
    if (e && *e && strlen(e) < sizeof v && strspn(e, "0123456789.") == strlen(e)) {
        snprintf(v, sizeof v, "%s", e);
        return v;
    }
    extern const char *g_native_lib_dir;
    char path[1024];
    snprintf(path, sizeof path, "%s/libOVRPlugin.so", g_native_lib_dir ? g_native_lib_dir : ".");
    return kl_ovrp_guest_version_in(path, v, sizeof v) ? v : NULL;
}

extern int kl_untested_profile(void);
static const char *klovrp_version_str(void) {
    static const char *v;
    if (!v) {
        const char *e = kl_env_str("KL_OVRP_VERSION", NULL);
        const char *t = kl_driver_target_name();
        if (e && *e)                                        v = e;
        else if (t && strcmp(t, "x2111191602278229") == 0)  v = "1.117.0";
        // BRINK 1.8.3 ships OVRPlugin 1.115.0; older versions skip OpenXR initialization.
        else if (t && strcmp(t, "x3635172946605196") == 0)  v = "1.115.0";
        else if (kl_guest_beatsaber_openxr())               v = "1.106.0";
        // Cubism 1.8.0 ships 1.108.0. The 1.60 default makes MetaXRFeature
        // reject OnInstanceCreate before entering the native OpenXR bridge
        // (owner's device log: black screen even with offline identity).
        else if (t && strcmp(t, "x2264524423619421") == 0)  v = "1.108.0";
        // Demeo ships 1.109.0 and has a mixed-reality mode. Its C# reaches the
        // passthrough entry points only from 1.63 on (init), 1.71 (supported),
        // 1.78/1.85 (capabilities); told 1.60.0 it never asks. With its own
        // number it asks for much else as well — see klovrp_soft_unanswered.
        else if (t && strcmp(t, "x3634830803298285") == 0)  v = "1.109.0";
        // Richie's Plank Experience ships 1.101.1 and has a mixed-reality mode it
        // asks for by itself, in its menu. Told 1.60.0 it reads "Passthrough
        // supported? False" off a capability question it believes the plugin too
        // old for, and its room stays black (on device, 2026-10-13). With its own
        // number it says True; on a Mac host run exactly one entry point came up
        // that had no answer (ovrp_IsControllerDrivenHandPosesEnabled, below).
        else if (t && strcmp(t, "x1642239225880682") == 0)  v = "1.101.1";
        // Synth Riders ships 1.115.0 (Oculus Utilities 1.115) and has a
        // mixed-reality mode that did nothing told 1.60.0 (2026-10-15).
        else if (t && strcmp(t, "x2436558143118760") == 0)  v = "1.115.0";
        // LES MILLS BODYCOMBAT ships 1.98.0 and has a mixed-reality switch in its
        // menu. Told 1.60.0, OVRPlugin's C# answers its passthrough request
        // itself — "Failed to initialize Insight Passthrough ... Failure_Unsupported"
        // without calling in — and the room stays black (on device, 2026-10-15).
        else if (t && strcmp(t, "x4015163475201433") == 0)  v = "1.98.0";
        // DreamSpace: Mixed Reality Home ships 1.203.0 and runs on Meta's OpenXR
        // backend. Told 1.60.0, its C# never calls ovrp_UnityOpenXR_OnInstanceCreate
        // ("[MetaXRFeature] OnInstanceCreate returned an error") and OVRPlugin
        // stays uninitialised (2026-10-09).
        else if (t && strcmp(t, "x9449316705178989") == 0)  v = "1.203.0";
        // The profile for untested games: the version the game ships, as every
        // title above was told by hand (kl_untested_profile; together with the
        // soft answer for entry points a newer plugin reaches).
        else if (kl_untested_profile() && (v = klovrp_guest_version()) != NULL)
            fprintf(stderr, "  [ovrp] OVRPlugin %s, as the game ships it (the profile for "
                            "untested games)\n", v);
        else                                                v = KLOVRP_VERSION_DEFAULT;
    }
    return v;
}
#define KLOVRP_VERSION (klovrp_version_str())

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

// ---------------------------------------------------------------------------
// OpenXR mode: OVRPlugin.UnityOpenXR (OVRP_1_71_0+)
//
// A guest on Unity's OpenXR plugin drives OVRPlugin through these instead of
// ovrp_Initialize: MetaXRFeature hands it the XrInstance / XrSession as they are
// created, and hooks xrGetInstanceProcAddr so the real plugin can call OpenXR
// itself. This plugin answers its ovrp_* queries from its own state and never
// calls OpenXR, so there is nothing to hook (the function comes back unchanged)
// and nothing to keep from the handles. What matters is the answer:
// OnInstanceCreate is ovrpResult, and anything but success makes MetaXRFeature
// log "OnInstanceCreate returned an error" and leave the Meta half of the SDK
// switched off. Instance creation is also when the real plugin starts reporting
// itself initialised, so ovrp_GetInitialized follows it here.
static uint64_t klovrp_UOX_SetClientVersion(uint64_t maj, uint64_t min, uint64_t patch) {
    ovrp_hit("ovrp_UnityOpenXR_SetClientVersion");
    fprintf(stderr, "  [ovrp] UnityOpenXR client %d.%d.%d (OpenXR mode)\n",
            (int)maj, (int)min, (int)patch);
    return 0;
}
static uint64_t klovrp_UOX_HookGetInstanceProcAddr(uint64_t fn) {
    ovrp_hit("ovrp_UnityOpenXR_HookGetInstanceProcAddr");
    return fn;                         // no hook: nothing here calls OpenXR
}
static uint64_t klovrp_UOX_OnInstanceCreate(uint64_t instance) {
    ovrp_hit("ovrp_UnityOpenXR_OnInstanceCreate");
    fprintf(stderr, "  [ovrp] UnityOpenXR OnInstanceCreate(%#llx) -> success\n",
            (unsigned long long)instance);
    g_ovrp_initialized = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_UOX_OnInstanceDestroy(uint64_t instance) {
    (void)instance;
    ovrp_hit("ovrp_UnityOpenXR_OnInstanceDestroy");
    g_ovrp_initialized = 0;
    return 0;
}
// The session lifecycle notifications. void in the SDK; nothing to record.
static uint64_t klovrp_UOX_OnSessionCreate(uint64_t a)     { (void)a; ovrp_hit("ovrp_UnityOpenXR_OnSessionCreate");  return 0; }
static uint64_t klovrp_UOX_OnSessionBegin(uint64_t a)      { (void)a; ovrp_hit("ovrp_UnityOpenXR_OnSessionBegin");   return 0; }
static uint64_t klovrp_UOX_OnSessionEnd(uint64_t a)        { (void)a; ovrp_hit("ovrp_UnityOpenXR_OnSessionEnd");     return 0; }
static uint64_t klovrp_UOX_OnSessionExiting(uint64_t a)    { (void)a; ovrp_hit("ovrp_UnityOpenXR_OnSessionExiting"); return 0; }
static uint64_t klovrp_UOX_OnSessionDestroy(uint64_t a)    { (void)a; ovrp_hit("ovrp_UnityOpenXR_OnSessionDestroy"); return 0; }
// The app space is recorded: ovrp_GetAppSpace hands it back (first argument of
// both forms in the real 1.203.0 plugin).
static uint64_t g_uox_app_space;
static uint64_t klovrp_UOX_OnAppSpaceChange(uint64_t a) {
    ovrp_hit("ovrp_UnityOpenXR_OnAppSpaceChange"); g_uox_app_space = a; return 0;
}
static uint64_t klovrp_UOX_OnAppSpaceChange2(uint64_t a, uint64_t f) {
    (void)f; ovrp_hit("ovrp_UnityOpenXR_OnAppSpaceChange2"); g_uox_app_space = a; return 0;
}
static uint64_t klovrp_UOX_OnSessionStateChange(uint64_t from, uint64_t to) {
    (void)from; (void)to; ovrp_hit("ovrp_UnityOpenXR_OnSessionStateChange"); return 0;
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
#define OVRP_HEADSET_META_QUEST_3   11   // ovrpSystemHeadset_Meta_Quest_3
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
        // Roboquest VR's Meta Store build (x9269393463181825) is presented as a
        // Quest 3 at its owner's request (kl_jni.c has why), and this answer
        // agrees with the model it was given there.
        // A Fisherman's Tale (UE4) is the same story told through the product
        // name: AInnerspacePlayerController::DetectDevices (libUE4 0x5f1ea60)
        // compares it with exactly "Oculus Quest" for Touch controllers, then
        // "Oculus Go", and anything else falls to head-gaze input with no hands
        // drawn. The name follows this answer (klovrp_product_name), so such a
        // title runs with KL_OVRP_HEADSET=8 (or a target default like Liminal's).
        const char *tgt = kl_driver_target_name();
        int dflt = (tgt && strcmp(tgt, "liminalvr") == 0)
                     ? OVRP_HEADSET_OCULUS_QUEST
                     : (tgt && strcmp(tgt, "x9269393463181825") == 0)
                     ? OVRP_HEADSET_META_QUEST_3
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
// this host, so the honest answer is 0 - except for Ghost Town, which
// recenters on a held right Options button (KleptonControllers.swift) and
// reads this count to learn that it happened. The compositor counts each host recenter (kl_ovrp_notify_recenter).
// From community PR #41 (sibest); KL_OVRP_RECENTER_COUNT=0/1 overrides.
static int g_host_recenter_count;
void kl_ovrp_notify_recenter(void) {
    __atomic_add_fetch(&g_host_recenter_count, 1, __ATOMIC_RELEASE);
}
static int klovrp_recenter_count_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_RECENTER_COUNT", t && !strcmp(t, "x9531494193591052"));
    }
    return v;
}
static uint64_t klovrp_GetLocalTrackingSpaceRecenterCount(int *out) {
    ovrp_hit("ovrp_GetLocalTrackingSpaceRecenterCount");
    if (!out) return -1001;
    *out = klovrp_recenter_count_on() ? __atomic_load_n(&g_host_recenter_count, __ATOMIC_ACQUIRE) : 0;
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
// The Array eye layout for a GLES guest: one GL_TEXTURE_2D_ARRAY per stage, a
// layer per eye, drawn single-pass through GL_OVR_multiview — and copied a layer
// at a time into the compositor's own eye textures at ovrp_EndFrame4, because an
// array texture cannot be re-pointed at an MTLTexture (kl_glfb_mirror_eye_layer;
// the same route an OpenXR guest's array swapchain takes).
//
// Per target, default off. A UE4 project cooked with vr.MobileMultiView asks for
// the Array layout whatever it is told here and has shaders for nothing else:
// Maestro (x6963344583787383) got a refusal, kept a NULL eye texture, and crashed
// in FMobileSceneRenderer::RenderForward on its first frame. twd2 is the other
// kind of UE guest — it takes DoubleWide when multiview reads false, and that is
// its proven path — so this is not switched on by engine. KL_OVRP_GL_ARRAY
// overrides in either direction; kl_egl.c offers GL_OVR_multiview to the same
// targets (klgl_want_multiview), since one without the other is a guest told it
// may render single-pass into storage it cannot attach.
int kl_ovrp_gl_array_for(const char *t) {
    {
        // Tetris Effect: Connected (x3386618894743567, UE 4.26) is the second:
        // refused, it set its eye layer up again in every frame until all 64
        // slots were eye layers, and stayed black on the headset (2026-10-13).
        // rebelancap's fork describes such a layer as Stereo instead and serves
        // the double-wide picture UE4 then draws; this runtime can hand out the
        // array the guest asked for, and with it the game has its picture and
        // plays (owner, headset, the same day).
        // Vader Immortal I–III (UE 4.19, cooked for mobile multiview) take
        // DoubleWide when told no multiview and boot, but then every LIT
        // material draws black: the console round the menu screen, the gloved
        // hands, the title logo's red (its emissive strength read a junk
        // value) — only the self-lit screen showed, on black, so there was
        // nothing to point with (his headset, 2026-10-19). With the Array
        // layer and real multiview the console and the hands are there (Mac).
        return kl_env_on("KL_OVRP_GL_ARRAY", t && (strcmp(t, "x6963344583787383") == 0 ||
                                                   strcmp(t, "x3386618894743567") == 0 ||
                                                   strcmp(t, "x2108775495884888") == 0 ||
                                                   strcmp(t, "x2223615721048141") == 0 ||
                                                   strcmp(t, "x2426206484098337") == 0));
    }
}

int kl_ovrp_gl_array(void) {
    static int v = -1;
    if (v < 0) v = kl_ovrp_gl_array_for(kl_driver_target_name());
    return v;
}

int kl_ovrp_multiview(void) {
    // GLES (ANGLE) guests have NO multiview and their eye layer is served only as
    // Stereo/DoubleWide — so multiview MUST read false for them, or a UE guest
    // (twd2) requests an Array eye layout that GetLayerTexture2 refuses on the GL
    // path, and the guest spins forever recalculating the layer it can never get.
    // Only the Vulkan path serves Array — and the GL targets named in
    // kl_ovrp_gl_array. (Dropping this guard once regressed twd2 into exactly
    // that non-booting CalculateLayerDesc loop.)
    if (!kl_vulkan_guest_active()) return kl_ovrp_gl_array();
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

// ovrp_GetTrackingPositionSupported2(ovrpBool* out) — Red Matter 2. Positional
// tracking IS supported (visionOS gives a full 6DoF head pose), so answer true.
static uint64_t klovrp_GetTrackingPositionSupported2(int *out) {
    ovrp_hit("ovrp_GetTrackingPositionSupported2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 1;
    return OVRP_SUCCESS;
}

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

// KL_OVRP_BOUNDARY=1|<w>,<h>,<d>: tell the guest a Guardian is set up, as on
// every Quest (a Quest cannot be used without one): configured, the given play
// area size in metres (default 2.0 x 2.5 x 2.0), and a rectangle of that size
// on the floor round the tracking origin as its geometry. Off, the answers say
// "no boundary" with zero dimensions, which no Quest ever reports. Angry Birds
// VR: Isle of Pigs fits its levels to the play area (levelScale,
// initialPhysicalLevelScaleUp; it calls ovrp_GetBoundaryDimensions) and its
// island did not stand in the room (2026-10-18). Default 1 for that target.
// Vader Immortal I–III (x2108775495884888, x2223615721048141,
// x2426206484098337) too: after the shader screen Episode I says the play
// area is too small and wants 2 x 2 m (2026-10-19).
//
// The size is the MEASURED play area once the start window's room measuring
// has one (kl_ovrp_set_play_area: the room's extent across the floor, or the
// stationary circle's), unless KL_OVRP_BOUNDARY names a size itself. A
// measured area also turns the answers on for every other game: a Quest with
// a Guardian is what every one of them was made for.
static uint64_t g_play_area;   // two floats, width and depth; 0 = none

void kl_ovrp_set_play_area(float width, float depth) {
    if (!(width > 0.0f && depth > 0.0f)) return;
    uint32_t a, b;
    memcpy(&a, &width, 4);
    memcpy(&b, &depth, 4);
    uint64_t v = (uint64_t)a | ((uint64_t)b << 32);
    if (__atomic_exchange_n(&g_play_area, v, __ATOMIC_RELAXED) != v)
        fprintf(stderr, "  [ovrp] Guardian: the measured play area is %.2f x %.2f m — "
                        "reported from now on\n", (double)width, (double)depth);
}

static int klovrp_boundary(float dims[3]) {
    static int v = -1, named = 0, off = 0;
    static float d[3] = { 2.0f, 2.5f, 2.0f };
    if (v < 0) {
        const char *t = kl_driver_target_name();
        const char *e = getenv("KL_OVRP_BOUNDARY");
        float w, h, z;
        if (e && sscanf(e, "%f,%f,%f", &w, &h, &z) == 3 && w > 0 && z > 0) {
            d[0] = w; d[1] = h; d[2] = z; v = 1; named = 1;
        } else {
            v = kl_env_on("KL_OVRP_BOUNDARY",
                          t && (!strcmp(t, "x2718606324833775") || !strcmp(t, "x2108775495884888") ||
                                !strcmp(t, "x2223615721048141") || !strcmp(t, "x2426206484098337")));
            off = e && !v;
        }
        if (v) fprintf(stderr, "  [ovrp] Guardian: reported as set up, play area %.2f x %.2f x %.2f m "
                               "(KL_OVRP_BOUNDARY)\n", d[0], d[1], d[2]);
    }
    uint64_t pa = named || off ? 0 : __atomic_load_n(&g_play_area, __ATOMIC_RELAXED);
    if (pa) {
        uint32_t a = (uint32_t)pa, b = (uint32_t)(pa >> 32);
        float w, z;
        memcpy(&w, &a, 4);
        memcpy(&z, &b, 4);
        if (dims) { dims[0] = w; dims[1] = d[1]; dims[2] = z; }
        return 1;
    }
    if (v && dims) { dims[0] = d[0]; dims[1] = d[1]; dims[2] = d[2]; }
    return v;
}

// Guardian boundary: there is no boundary system on this host, so the
// configured flag is false — same stance as the Geometry2/Visible forms. Real
// plugin is ovrpResult + int* out (0x171170).
static uint64_t klovrp_GetBoundaryConfigured2(int *out) {
    ovrp_hit("ovrp_GetBoundaryConfigured2");
    if (out) *out = klovrp_boundary(NULL);
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
// Environment depth (Quest 3 passthrough occlusion). There is no depth pipe on
// visionOS, so no frame is ever valid — and that has to be WRITTEN: the
// descriptor's first field is its IsValid, and Meta's Unreal plugin hands in an
// uninitialised local and reads it back after a success. Left untouched, Alien:
// Rogue Incursion found a stale stack-guard value there on its first frame,
// took the frame for valid and indexed its depth swapchain with more of the
// same (SIGSEGV on RenderThread, 2026-10-09). wanderer's local happens to be
// zero, which is why "success and touch nothing" was enough for it. Only that
// one field is written; the rest of the struct is not read behind a false.
static uint64_t klovrp_GetEnvironmentDepthFrameDesc(int eye, int *desc) {
    ovrp_hit("ovrp_GetEnvironmentDepthFrameDesc");
    (void)eye;
    if (desc) *desc = 0;    // ovrpEnvironmentDepthFrameDesc.IsValid = ovrpBool_False
    return OVRP_SUCCESS;
}
// KL_OVRP_NO_TRACKERS: answer the four tracker nodes (TrackerZero..Three,
// 5..8 — the Rift's external sensors) the way a Quest does: not present, not
// tracked, at the origin. They used to report the HEAD pose like every other
// unmapped node, which tells a guest that a sensor rides on the user's head.
// Angry Birds VR: Isle of Pigs reads TrackerZero first thing from its C#, and
// its island followed the player instead of standing in the room — turning
// changed the view, walking only changed the angle (2026-10-18). Per target
// for now; nothing measured says another title reads these nodes.
static int klovrp_no_trackers(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_NO_TRACKERS", t && !strcmp(t, "x2718606324833775"));
        if (v) fprintf(stderr, "  [ovrp] tracker nodes 5..8: not present, at the origin "
                               "(KL_OVRP_NO_TRACKERS)\n");
    }
    return v;
}
static int klovrp_is_tracker(int node) { return node >= 5 && node <= 8; }

static uint64_t klovrp_GetNodePresent2(int node, int *out) {
    ovrp_hit("ovrp_GetNodePresent2");
    // node comes FIRST (real 0x16f450: w0 = ovrpNode, x1 = int* out) — the
    // last "write a bool" shape this guest has that is NOT a single
    // pointer. Any node a caller names is one we report; the value is written
    // for the callers that read it, since at least one (OculusSystem::
    // GetNodePresent, libOculusXRPlugin+0xf264) only checks the return.
    if (out) *out = !(klovrp_is_tracker(node) && klovrp_no_trackers());
    return OVRP_SUCCESS;
}

// Another node-first predicate, real 0x16f4b0 is the same (int node, int *out)
// shape as GetNodePresent2 — w0=node, x1=out. Orientation IS tracked for
// every node we report poses for.
static uint64_t klovrp_GetNodeOrientationTracked2(int node, int *out) {
    ovrp_hit("ovrp_GetNodeOrientationTracked2");
    if (out) *out = !(klovrp_is_tracker(node) && klovrp_no_trackers());
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
//
// LES MILLS BODYCOMBAT is told "Oculus Quest", a name Meta's own plugin does
// return (strings(1) on the 1.98.0 it ships lists "Oculus Quest",
// "Oculus Quest2", "Meta Quest Pro"). The game picks the pose of its pointer
// ray, hand model and wrist watch by the HMD's InputDevice name — which the
// Oculus XR plugin takes from here — out of a table whose Quest entries are
// "Oculus Quest" and "Oculus Headset2". "Oculus Quest 2" matched neither, the
// game fell back to its default settings, and on device (2026-10-15) the ray
// pointed straight down and the watch sat upside down above the hand.
//
// A title asked with KL_OVRP_HEADSET=8 to run as an original Quest gets that
// Quest's name too: a guest that keys on the NAME (A Fisherman's Tale's
// DetectDevices) would otherwise still see a headset it does not know. Only the
// asked-for case: Liminal's built-in 8 keeps the name it has always had here.
static const char *klovrp_product_name(void) {
    static const char *v;
    if (!v) {
        const char *t = kl_driver_target_name();
        int quest1 = t && strcmp(t, "x4015163475201433") == 0;
        if (kl_env_str("KL_OVRP_HEADSET", NULL) && klovrp_headset_type() == OVRP_HEADSET_OCULUS_QUEST)
            quest1 = 1;
        v = quest1 ? "Oculus Quest" : "Oculus Quest 2";
    }
    return v;
}
static const char *klovrp_GetSystemProductName(void) {
    ovrp_hit("ovrp_GetSystemProductName");
    return klovrp_product_name();
}

// The 2-form of the same answer: ovrpResult + const char** out — real
// 0x170700 writes the string POINTER through [out], it is not a scalar
// return. Same "Oculus Quest 2" as the un-suffixed form; the two product-
// name surfaces must never disagree, and a static literal is never freed.
static uint64_t klovrp_GetSystemProductName2(const char **out) {
    ovrp_hit("ovrp_GetSystemProductName2");
    if (!out) return -1001;
    *out = klovrp_product_name();
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

// KL_DISPLAY_HZ, resolved once: >0 means the rate was FORCED and must outrank
// the compositor's live measurement. It used not to: the getter set g_display_hz
// from the env, and then the compositor's priming pass called the setter with
// the number it measured over its first frames, silently overwriting it. That
// is fine for a rate the guest tolerates, and it is a crash for Steam Link's VR
// client — libvrlink_scene's QSVLClient::UpdateFrameVariables (+0xf14e0) takes
// an empty-container branch for the display-rate/frametime submission and then
// dereferences its null data pointer regardless (VTE_AVAILABLE_FRAMETIMES_US).
// So a run has to be able to PIN the rate the guest is told, which is what the
// forced flag protects — the measurement can no longer clobber the override.
// A driver may PIN the rate for a guest that is known to mishandle a measured
// one — Steam Link's VR client crashes on anything but a value it was told up
// front — without the run having to pass KL_DISPLAY_HZ. The env still wins.
static float g_forced_hz_hint;
void kl_ovrp_set_forced_hz_hint(float hz) { g_forced_hz_hint = hz; }

static float klovrp_forced_hz(void) {
    static int   checked;
    static float forced;   // 0 = not forced
    if (!checked) {
        checked = 1;
        const char *e = getenv("KL_DISPLAY_HZ");
        if (e && *e) {
            float hz = strtof(e, NULL);
            if (hz >= 30.0f && hz <= 240.0f) {
                fprintf(stderr, "  [ovrp] display frequency forced to %.1f Hz "
                                "by KL_DISPLAY_HZ (measurement will not override)\n",
                        (double)hz);
                forced = hz;
            }
        } else if (g_forced_hz_hint >= 30.0f && g_forced_hz_hint <= 240.0f) {
            fprintf(stderr, "  [ovrp] display frequency pinned to %.1f Hz for this "
                            "target (measurement will not override; KL_DISPLAY_HZ "
                            "overrides)\n", (double)g_forced_hz_hint);
            forced = g_forced_hz_hint;
        }
    }
    return forced;
}

void kl_ovrp_set_display_frequency(float hz) {
    // An explicit override wins over a measurement — the whole point of forcing
    // a rate is a guest that mishandles the real one, so letting the compositor's
    // measured value overwrite it here would undo exactly the fix it is for.
    if (klovrp_forced_hz() > 0.0f) return;
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
// the other end of the wire is asking for". On device it is the override that
// outranks the compositor's live measurement — see klovrp_forced_hz.
float kl_ovrp_display_frequency(void) {
    float forced = klovrp_forced_hz();
    if (forced > 0.0f) g_display_hz = forced;
    return g_display_hz;
}

// ---- battery telemetry seam ----
// One source of truth for the battery, routed through by the OVRPlugin query
// (GetSystemBatteryLevel2), the Java BatteryManager answer (kl_jni.c), and —
// on device — a frontend pushing UIDevice's real reading. Sanity checks mirror
// the display-frequency setter: a level outside 0..100 is a bad measurement,
// and passing it on would make every consumer answer nonsense.
static int g_battery_level = 95;       // host/unknown fallback; visionOS publishes UIDevice
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

// PSVR2 Sense controllers are two independent GCController devices, so retain
// two independent readings. The render thread reads these while GameController
// is polled by the frontend; atomics keep that seam valid if either callback is
// ever moved off the render thread. Unknown is distinct from 0%: OpenXR has a
// validity bit, while OVRPlugin's byte has no unknown representation and keeps
// its historical 100% fallback in fill_controller_state below.
static int g_controller_battery_level[2] = { -1, -1 };
static int g_controller_battery_state[2] = {
    KL_CONTROLLER_BATTERY_UNKNOWN, KL_CONTROLLER_BATTERY_UNKNOWN
};

void kl_ovrp_set_controller_battery(int hand, int level, int state) {
    if (hand < 0 || hand > 1) return;
    if (level < 0) level = -1;
    else if (level > 100) level = 100;
    if (state < KL_CONTROLLER_BATTERY_UNKNOWN ||
        state > KL_CONTROLLER_BATTERY_FULL)
        state = KL_CONTROLLER_BATTERY_UNKNOWN;
    __atomic_store_n(&g_controller_battery_level[hand], level, __ATOMIC_RELEASE);
    __atomic_store_n(&g_controller_battery_state[hand], state, __ATOMIC_RELEASE);
}

int kl_ovrp_controller_battery_level(int hand) {
    if (hand < 0 || hand > 1) return -1;
    return __atomic_load_n(&g_controller_battery_level[hand], __ATOMIC_ACQUIRE);
}

int kl_ovrp_controller_battery_state(int hand) {
    if (hand < 0 || hand > 1) return KL_CONTROLLER_BATTERY_UNKNOWN;
    return __atomic_load_n(&g_controller_battery_state[hand], __ATOMIC_ACQUIRE);
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

// KL_OVRP_ADAPTIVE_RES — the eye layer's recommended resolution follows the GPU.
//
// Meta's dynamic resolution is the runtime telling the application, frame by
// frame, how large an eye picture the GPU can afford
// (ovrp_GetLayerRecommendedResolution), and the application moving its render
// viewport toward that inside the eye texture it already has. Answered with the
// full size every time, a title built on it renders at its maximum whatever
// that costs: Alien: Rogue Incursion sat at 3028x2464 an eye and 16-20 frames a
// second outdoors, the GPU taking 45-50 ms a frame (2026-10-09).
//
// The load is the time the guest's frame still had to go on the GPU when the
// guest called it done (measured at frame end, below) — near zero for a guest
// the GPU keeps up with, most of the frame for one it does not. Averaged over
// 30 frames; the scale goes down by the square root of the overshoot (GPU time
// follows the pixel count), at most 8 % a step and never below 70 %, and back
// up 3 % a step once there is room. The application smooths it on its side.
//   KL_OVRP_ADAPTIVE_RES=0|1     default 1 for that title, else 0
//   KL_OVRP_ADAPTIVE_GPU_MS=<ms> the wait it steers toward, default 14
static float g_adapt_scale = 1.0f;
static int klovrp_adapt_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_OVRP_ADAPTIVE_RES", t && !strcmp(t, "x24882487241367228"));
    }
    return on;
}
static void klovrp_adapt_note(double wait_ms) {
    if (!klovrp_adapt_on()) return;
    static double sum;
    static unsigned n, changes;
    static float target = -1.0f;
    if (target < 0) {
        target = kl_env_float("KL_OVRP_ADAPTIVE_GPU_MS", 14.0f);
        if (!(target >= 2.0f && target <= 100.0f)) target = 14.0f;
    }
    sum += wait_ms;
    if (++n < 30) return;
    double mean = sum / n;
    sum = 0; n = 0;
    float s = g_adapt_scale;
    if (mean > target * 1.15) {
        float k = sqrtf(target / (float)mean);
        s *= k < 0.92f ? 0.92f : k;
    } else if (mean < target * 0.80 && s < 1.0f) {
        s *= 1.03f;
    }
    if (s > 1.0f) s = 1.0f;
    if (s < 0.70f) s = 0.70f;
    if (s == g_adapt_scale) return;
    g_adapt_scale = s;
    if (changes++ < 12 || changes % 50 == 0)
        fprintf(stderr, "  [ovrp] adaptive resolution: the GPU had %.1f ms to go at frame end "
                        "(target %.0f) — recommending %.0f%% of the eye size (change %u)\n",
                mean, (double)target, (double)s * 100.0, changes);
}

// The per-eye render target size. Quest 2's recommendation until a frontend
// measures the display it is actually running on — see kl_ovrp.h.
static int g_eye_w = (int)KL_OVRP_EYE_W;
static int g_eye_h = (int)KL_OVRP_EYE_H;

// What KL_OVRP_RENDER_SCALE defaults to: the frontend's stored setting, itself
// defaulting to the target's own (targets.py "render_scale").
static float g_render_scale_default = 1.0f;
void kl_ovrp_set_render_scale_default(float s) {
    if (s >= 0.2f && s <= 1.0f) g_render_scale_default = s;
}

// The cap the size below was held to, for klovrp_fill_eye_desc (KL_OVRP_EYE_DESC_CAP).
// -1 until a size is pushed: 0 is an answer (no cap: Native, or KL_OVRP_EYE_MAX=0).
static int g_eye_cap = -1;

// What g_eye_w/h were made of: the size after the cap and the render scale
// applied to it (kl_ovrp_eye_texture_scale). 0 / 1.0 until a frontend sets one.
// Written where the display's size is pushed and read by the compositor every
// frame and by the guest's threads, elsewhere — under their own lock, together
// with g_eye_w/h, so a reader gets one answer (a size with the scale and the
// size it was taken from), never a torn mix. Only ever held for the copies.
static pthread_mutex_t g_eye_scale_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_eye_full_w, g_eye_full_h;
static float g_eye_rs = 1.0f;

// KL_OVRP_EYE_ALIGN=<n> (1 = off): each axis of the eye size is rounded DOWN to a multiple of n, last of all.
// Unreal quantizes its scene buffers UP to a multiple of 4, and every foveation match in kl_vulkan.c compares
// extents exactly: Moss drew a 3840x2468 scene for a 3840x2465 eye, and kl_fove left its eye pass unmarked
// ("near miss … at ANOTHER extent", headset log of 2026-10-13) – foveated rendering was on and did nothing.
// An eye that is already a multiple of 4 is what the guest's own buffers come out at. From rebelancap's fork
// (be18669), where it is a per-title setting; here 4 for Moss, off elsewhere until seen.
static int klovrp_eye_align_n(void) {
    static int n = -1;
    if (n < 0) {
        n = kl_env_int("KL_OVRP_EYE_ALIGN", kl_driver_target_is_moss() ? 4 : 1);
        if (n < 1 || n > 64) n = 1;
    }
    return n;
}
static void klovrp_eye_align(int *w, int *h) {
    int n = klovrp_eye_align_n();
    if (n <= 1) return;
    if (*w >= n) *w -= *w % n;
    if (*h >= n) *h -= *h % n;
}
// The eye cap a title is tuned for, without the player's choice or the launch line:
// what the start window shows as "Default (…)" (kl_ovrp_title_eye_cap). By name, since
// the window asks before the guest is loaded; the rows that look at the guest itself
// (Beat Saber from 1.45 on) only answer once it is.
static int klovrp_title_cap(const char *t) {
    // Per-target default. Doom3Quest lights in passes — every light re-shades
    // the pixels it touches — and squares and supersamples whatever it is told
    // (3072x2464 became 3379x3379 per eye, 22.8 MP a frame, against ~5 MP on the
    // Quest 2 it was tuned for). Its cap was 2048 for that reason (about
    // 2250x2250 per eye after its own 1.1x), which on this display is roughly 19
    // pixels per degree and reads as a coarse picture. Measured on device,
    // 2026-10-08: at 4000 (4400x4400 per eye) the owner called the picture good
    // and the guest ran at 53-55 frames a second falling to 31-35 over three
    // minutes. So the CEILING is 4000 and the target's render scale
    // (targets.py "render_scale", the "Render resolution" setting) picks the
    // point under it: 0.5 is the old picture, 1.0 the sharp one. Foveation
    // cannot help yet — this guest renders into an array swapchain of its own
    // (see kl_glfb_set_eye_rate_map).
    int cap_dflt = 3072;
    {
        if (t && strcmp(t, "doom3quest") == 0) cap_dflt = 4000;
        // AC Nexus: a quarter more, paid for by rendering 2x multisampled
        // instead of 4x (KL_VK_MAX_SAMPLES in kl_vulkan.c, which is where the
        // memory arithmetic is). 3072 px is about 22 pixels a degree at the
        // centre of this display, and its owner read that as "no anti-aliasing
        // or too little resolution"; at 3840 he called it good enough.
        if (t && strcmp(t, "x5812519008825194") == 0) cap_dflt = 3840;
        // Counter-Strike VR squares what it is told and adds its own 1.1
        // (vr_supersampling): 3072 became 3379x3379 per eye, 22.8 MP a frame
        // against about 10 on the Quest 3 it was tuned for. On device,
        // 2026-10-11, the COMPOSITOR then presented every 21 to 29 ms on a 100 Hz
        // panel (its own pass queued 26 to 38 ms behind the guest's and ran 11 to
        // 17), which the player described as the world following a head turn
        // with a slight delay — a picture that is itself updated at 35 to 48 Hz.
        // 2048 gave it 2253x2253, a Quest's load — and it turned out that half
        // of the trouble had been the guest's unpaced frame loop (kl_app.c), not
        // the size. Paced, 2253x2253 ran at the panel's rate with the compositor's
        // pass queued under a millisecond, and its player asked for a sharper
        // picture: 2560 gives 2816x2816.
        if (t && strcmp(t, "cs1") == 0) cap_dflt = 2560;
        // Maestro: Unreal on GL asks for 4x multisampled multiview, which this
        // backend can only give it single-sampled, and it then renders at 0.9
        // of whatever it is told — so at the usual cap the picture is both
        // aliased and under the display's own density (its owner: "the
        // resolution could be higher"). A light scene; the larger target is
        // the anti-aliasing it has.
        if (t && strcmp(t, "x6963344583787383") == 0) cap_dflt = 4000;
        // Tetris Effect: the same Unreal-on-GL array path, and a lighter scene
        // still — at 3072 it held every frame of a 100 Hz panel with the
        // composite queued half a millisecond, 2.7 GB of 8.1 in use (headset,
        // 2026-10-13), and its owner asked for more. The display's own size:
        // 4633x3717 held too — 5 of 9456 frames late, 3.2 GB in use.
        if (t && strcmp(t, "x3386618894743567") == 0) cap_dflt = 4640;
        // TRIANGLE STRATEGY: a tabletop world looked at from close by, where
        // 3072 read as too coarse ("Die Auflösung könnte höher sein",
        // 2026-10-12, in its mixed-reality mode). At 3072x2464 with its own 4x
        // multisampling the compositor kept 100 Hz with its pass queued 0.2 ms
        // behind the game's, so there is room.
        if (t && strcmp(t, "x5340130476095884") == 0) cap_dflt = 4000;
        // Demeo, the same kind of picture — a board on a table, looked at from
        // an arm's length — and the same wish ("Kann man die Auflösung noch nach
        // oben setzen?", 2026-10-13). At 3072 its plugin's widened eye was
        // 3936x2465, 1136 pixels a unit of tangent where the display's light
        // output shows 1696; 4000 makes that about 1480. Paid for with the 2x
        // multisampling it is held at (KL_VK_MAX_SAMPLES): at 3072 and 2x the app
        // stood at 4.4 to 4.7 of its 8.2 GB.
        if (t && strcmp(t, "x3634830803298285") == 0) cap_dflt = 4000;
        // Synth Riders (Unity 6000, fix 39): its owner on the first build with a
        // picture, 2026-10-15: "Auflösung könnte höher sein". At 3072 its eye was
        // 3072x2465, and its anti-aliasing is gone (KL_GLFB_MS_TEX_AS_2D serves
        // the 4x scene target single-sampled), so the larger target is what
        // smooths its edges; 4000 as for the two games above.
        if (t && strcmp(t, "x2436558143118760") == 0) cap_dflt = 4000;
        // LES MILLS BODYCOMBAT: a light scene on GL with no multisampling of its
        // own. At 3072 (the display asked for 4633) its owner called the picture
        // "viel zu niedrig" on device, 2026-10-15.
        if (t && strcmp(t, "x4015163475201433") == 0) cap_dflt = 4000;
        // Gun Club VR (Unity on GL, fix 44): its owner on the first build that
        // ran well, toolchain 555, 2026-10-15: could the resolution go higher?
        // At 3072 (the display asked for about 4633x3717) its eye was 3072x2465,
        // and the "Resolution" setting only scales down from there, so the cap is
        // the one lever. 4000 as for the other Unity GL titles above; the setting
        // (or KL_OVRP_EYE_MAX) takes it back down if a busy scene stutters.
        if (t && strcmp(t, "x2285803881494677") == 0) cap_dflt = 4000;
        // Catan VR (Unity 2018.4 on GL) and LEGO Bricktales (Unity 6000.2 on
        // Vulkan): their owner asked for 4000 as the default, 2026-10-18. LEGO
        // scales its eye by 1.41 itself and is widened for symmetric
        // projection on top, so its eye grows well past 4000; the start
        // window's "Resolution" takes it back down if memory runs short.
        if (t && (strcmp(t, "x3882524545155280") == 0 || strcmp(t, "x6521909757843713") == 0))
            cap_dflt = 4000;
        // Medal of Honor: Above and Beyond (UE4 on Vulkan): its owner on the
        // first run that started, 2026-10-18, chose 4000 in the start window
        // and asked for it as the default.
        if (t && strcmp(t, "x3647259232032222") == 0) cap_dflt = 4000;
        // Forest - Oniri tech demo: its owner asked for no limit as the
        // default, 2026-10-18, when it was verified. 0 is no cap, the same as
        // "Native" in the start window: the whole size the display asks for.
        if (t && strcmp(t, "x7611090302277055") == 0) cap_dflt = 0;
        // PowerWash Simulator VR (Unity 2020.3 on Vulkan): its owner played it
        // at 4000 from the start window, 2026-10-19, and asked for it as the
        // default with a frame on every second refresh (targets.py
        // STORE_DEFAULTS). At 4608 its eye was 4512x3697 and its GPU time a
        // frame 8 to 11 ms.
        if (t && strcmp(t, "x3713270985434472") == 0) cap_dflt = 4000;
        // Floor Plan 2 (Unity 2022.3 on Vulkan): its owner chose 4096 in the
        // start window and asked for it as the default when it was verified,
        // 2026-10-18 (7-8 ms of GPU a frame at 60 a second there).
        if (t && strcmp(t, "x2401648309868679") == 0) cap_dflt = 4096;
        // HITMAN 3 VR: Reloaded (Glacier on Vulkan, OpenXR): 3584, with 100 Hz, a
        // frame on every second refresh and foveation at "high" (targets.py
        // STORE_DEFAULTS). 4096 was its owner's first choice, and at it the game
        // ended at level changes: 6.5 GB held in a level, 7.7 of the ~8.2 the
        // app may have while the next one loaded. At 3072 it held 5.3-5.7 GB
        // and did not (headset, 2026-10-20); he asked for one step down.
        if (t && strcmp(t, "x8012387035448196") == 0) cap_dflt = 3584;
        // Star Wars Pinball VR (UE 4.25 on GLES): its owner chose 4608 in the
        // start window and asked for it as the default, 2026-10-19, with 120 Hz
        // and a frame on every second refresh (targets.py STORE_DEFAULTS). On
        // the headset: 59.3 shown of 119.5 Hz, about 3 ms of game GPU a frame.
        if (t && strcmp(t, "x3180994211970184") == 0) cap_dflt = 4608;
        // Vader Immortal I-III (UE 4.19 on GLES): its owner asked for 4096 as the
        // default once Episode I was verified, 2026-10-19, with 120 Hz and a
        // frame on every second refresh (targets.py STORE_DEFAULTS). II and III
        // are the same engine build and take the same, not yet run here.
        // Then the owner's later choice for Episode I, read off his headset
        // (2026-10-20): 4608 an eye, rendered at 95 % and brought back to the
        // full eye size by MetalFX (targets.py); II and III take the same at
        // his word.
        if (t && (strcmp(t, "x2108775495884888") == 0 || strcmp(t, "x2223615721048141") == 0 ||
                  strcmp(t, "x2426206484098337") == 0))
            cap_dflt = 4608;
        // Batman: Arkham Shadow: 4608, the eye size rebelancap runs it at with
        // foveation "high" (kl_fove) — see the note on its row in
        // visionos/targets.py. Unfoveated he measured 4032 as its rung and 5120
        // as too much (a third of the frame rate, 6.3 GB peak).
        if (t && strcmp(t, "batman") == 0) cap_dflt = 4608;
        // Moss: the display's own size, with foveated rendering at "high" (targets.py) and the game's own
        // 1.5x held to it (KL_OVRP_EYE_DESC_CAP). The owner's setting on the headset, 2026-10-13: "hoch ist
        // definitiv besser"; 8 ms of GPU a frame.
        if (kl_driver_target_is_moss()) cap_dflt = 4608;
        // Beat Saber from 1.45 on (kl_guest_beatsaber_openxr) is the same case
        // in Unity: its camera target is a 4x multisampled multiview array,
        // which is attached single-sampled here (klgl_want_msrtt in kl_egl.c),
        // so the game that had anti-aliasing as 1.40 has none. Its owner on
        // device, 2026-10-12: "Treppchen" — stair-stepped edges — at 3072, a
        // better picture and a smooth one at 4000, and a slight judder at 5000.
        if (kl_guest_beatsaber_openxr()) cap_dflt = 4000;
        // ...and with the guest's own pass foveated through the display's map
        // (klfb_mirror_vrr) the same game can be told the display's size
        // itself. The display asks for 6888x5525 at full quality and stores
        // 2624x2560 of it; this guest renders a level at 1.2 times what it is
        // told, so 5740 makes a level exactly the display's screen size — one
        // guest fragment for every fragment the drawable keeps, a third of the
        // fragments 4800x3850 unfoveated cost. The map is the display's own
        // FIXED one: measured on device (2026-10-12, KL_CP_GAZE_TRACE), its
        // dense region does not move with the eyes.
        extern int kl_glfb_mirror_foveation_wanted(void);
        if (kl_guest_beatsaber_openxr() && kl_glfb_mirror_foveation_wanted()) cap_dflt = 5740;
        // Doom 3 likewise, a step lower: it squares and supersamples what it is
        // told (5200 becomes 5720x5720 an eye), which at this cap is the
        // display's own height and five sixths of its width. Its owner asked
        // for more than the 4000 it had ("Bei Foveated rendering sollte man
        // höher gehen können"). Both eyes are drawn through the envelope of the
        // display's two curves (kl_glfb_rate_map_per_eye), which keeps about a
        // third of the fragments: some 10 million an eye here against 6 at
        // 4400x4400 — and 19 million for that size unfoveated, which is what
        // this game was first tuned against. "Resolution" in the start window
        // scales it down.
        if (t && strcmp(t, "doom3quest") == 0 && kl_glfb_mirror_foveation_wanted()) cap_dflt = 5200;
    }
    return cap_dflt;
}
int kl_ovrp_title_eye_cap(const char *target) {
    return klovrp_title_cap(target ? target : kl_driver_target_name());
}

// "Max resolution per eye" in the start window (KleptonEyeMax.swift): 0 is the title's
// own cap, KL_OVRP_EYE_NATIVE none at all (the display's whole request), anything else
// that many pixels on the longer axis. Under KL_OVRP_EYE_MAX, so a launch line still wins.
static int g_eye_max_choice;
void kl_ovrp_set_eye_max_choice(int px) {
    g_eye_max_choice = px > 0 || px == KL_OVRP_EYE_NATIVE ? px : 0;
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
    // Launch line > the player's choice > the title's cap (3072 unless its row above
    // says otherwise). Native is cap 0, which the clamp below leaves alone.
    int cap_dflt = g_eye_max_choice == KL_OVRP_EYE_NATIVE ? 0
                 : g_eye_max_choice > 0 ? g_eye_max_choice : klovrp_title_cap(kl_driver_target_name());
    int cap = kl_env_int("KL_OVRP_EYE_MAX", cap_dflt);
    g_eye_cap = cap;
    if (cap > 0 && (w > cap || h > cap)) {
        // Preserve the aspect ratio: clamping the axes independently would
        // change the shape of the picture, and the frustum tangents pushed
        // alongside this would then disagree with it.
        float k = (float)cap / (float)(w > h ? w : h);
        w = (int)((float)w * k + 0.5f);
        h = (int)((float)h * k + 0.5f);
    }
    // The person's (or the target's) render scale, applied AFTER the cap so it
    // always means "this fraction of what would otherwise be rendered" — the
    // pre-cap KL_OVRP_EYE_SCALE above cannot do that on a display that asks for
    // twice the cap, where 0.75 of the request is still clamped to the same
    // size. This is the fill-rate lever for a Vulkan guest: its own fixed
    // foveation needs a fragment density map MoltenVK does not offer ("Foveation
    // is not supported" in Arkham Shadow's log), so every pixel of the eye
    // texture is shaded at full rate, and a guest that saturates the GPU takes
    // the compositor's 100 Hz down to 50 with it — each reprojection shown
    // twice, which is judder on every head turn. Kept even so the viewport
    // halves cleanly.
    float rs = kl_env_float("KL_OVRP_RENDER_SCALE", g_render_scale_default);
    if (!(rs >= 0.2f && rs <= 1.0f)) rs = 1.0f;   // the same floor as the setter's
    const int full_w = w, full_h = h;
    if (rs < 1.0f) {
        w = ((int)((float)w * rs + 0.5f) + 1) & ~1;
        h = ((int)((float)h * rs + 0.5f) + 1) & ~1;
    }
    klovrp_eye_align(&w, &h);
    // One write for all five: a call that lands on the same size still says
    // what that size was made of (kl_ovrp_eye_texture_scale).
    pthread_mutex_lock(&g_eye_scale_mu);
    const int was_w = g_eye_w, was_h = g_eye_h;
    g_eye_full_w = full_w;
    g_eye_full_h = full_h;
    g_eye_rs = rs;
    g_eye_w = w;
    g_eye_h = h;
    pthread_mutex_unlock(&g_eye_scale_mu);
    if (w == was_w && h == was_h) return;

    // Logged every change, not once: each one costs the guest a swapchain
    // rebuild, and how many of those happen is the question. The MiB figure is
    // the whole swapchain Unity will hold.
    double mib = (double)w * h * 8.0 * 2.0 * kl_ovrp_stage_count() / (1024 * 1024);
    fprintf(stderr, "  [ovrp] eye texture size %dx%d -> %dx%d; the display asked "
                    "for %dx%d (scale %.2f, cap %d, render scale %.2f). %.0f MiB "
                    "of swapchain, before Unity's own ~1.2x\n",
            was_w, was_h, w, h, req_w, req_h, scale, cap, rs, mib);
}

void kl_ovrp_eye_texture_size(int *w, int *h) {
    pthread_mutex_lock(&g_eye_scale_mu);
    if (w) *w = g_eye_w;
    if (h) *h = g_eye_h;
    pthread_mutex_unlock(&g_eye_scale_mu);
}

void kl_ovrp_eye_texture_scale(float *rs, int *full_w, int *full_h) {
    pthread_mutex_lock(&g_eye_scale_mu);
    if (rs) *rs = g_eye_rs;
    if (full_w) *full_w = g_eye_full_w;
    if (full_h) *full_h = g_eye_full_h;
    pthread_mutex_unlock(&g_eye_scale_mu);
}

// Packed return: width in the low 32 bits, height in the high (0x9bce58).
static uint64_t klovrp_GetEyeTextureSize(void) {
    ovrp_hit("ovrp_GetEyeTextureSize");
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    return (uint64_t)(uint32_t)w | ((uint64_t)(uint32_t)h << 32);
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
//
// Walkabout is the same shape by a different route: its OculusXR (Unity 6) widens
// the eye texture itself for Meta's symmetric projection (3072 -> 3936 on device),
// renders both eyes with that one symmetric cone, and asks
// ovrp_CalculateEyeViewportRect where each eye goes — which answers "the whole
// texture", so both eyes arrive at 0,0 full-width (device log: `render viewport:
// eye0 0,0 3936x2464 eye1 0,0 3936x2464`). User-reported on device as the same
// picture in both eyes with no usable stereo.
static int klovrp_unify_default(void) {
    // Half-Life: Alyx was on this list for a day. Given two mirrored cones it
    // drew both eyes with the left one — but that is its own symmetric-FOV mode
    // misfiring without a Qualcomm extension, and `+vr_symmetric_fov 0`
    // (kl_linuxproc.c) switches the mode off: each eye is then drawn with its
    // own cone and a fifth of the picture's width is no longer drawn for
    // nothing. KL_OVRP_UNIFY_FRUSTUM=1 with the game's mode left on is the old
    // arrangement, should it be wanted for comparison.
    // Demeo (x3634830803298285, Unity 6, the same OculusXR generation) is
    // Walkabout's case to the pixel — `SetupLayer -> EYE, 3936x2465`, `render
    // viewport: eye0 0,0 3936x2465 eye1 0,0 3936x2465`, "Symmetric Projection
    // - True" in its own log — and its owner on device, 2026-10-12: the two
    // eyes show no picture that can be laid over one another.
    // Ghost Town uses the same widened eye texture and full-width viewport
    // for both eyes (3936x2465, then 5088x3204 with its resolution scale).
    // Match the compositor's cones to that symmetric-projection rendering.
    // HITMAN 3 VR: Reloaded (Glacier, OpenXR) does Meta's arithmetic itself: told
    // the two canted cones it made its swapchain wider by their union (4096 ->
    // 5193, x1.268 = 2 tan 60 / (tan 60 + tan 45)), drew both eyes through the
    // left eye's viewport and frustum (no per-view viewports here, KL_XR_VIEW0_RECT)
    // and so left the right eye's outer 15 degrees undrawn, while the start
    // window said 4096 (the owner on device, 2026-10-20: "die auflösung ist 4096
    // aber er rendert 5193"). Told the union, it draws each eye full width with
    // one cone: the area the setting names, both eyes complete.
    // Contractors (x2436897736439055, UE 4.27 Oculus) is Walkabout's shape from
    // Unreal: its 3379-wide eye becomes a 4224 texture (x1.25), and `render
    // viewport: eye0 0,0 4224x2710 eye1 0,0 4224x2710`. On device each eye alone
    // looked right and the two together did not overlay (2026-10-28).
    static const char *on[] = { "intotheradius", "x5817243991680545", "walkabout", "x3634830803298285", "x9531494193591052",
                                "x8012387035448196", "x2436897736439055" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    // LEGO Bricktales (Unity 6000.2) is the same case again (3936x2465, both
    // eyes full width; on device "the same scene, shifted" between the eyes,
    // 2026-10-17). The union is consistent for any guest — what it is told is
    // what the composite places — and costs only some density at the edge, so
    // the profile for untested games takes it for every Unity title that ships
    // Meta's OculusXR plugin, the one that renders with symmetric projection
    // (Catan VR, legacy VR, keeps the cones it was confirmed with).
    // Not Angry Birds VR: Isle of Pigs (Unity 6000.0, an older OculusXR). Its
    // plugin places each eye in the texture itself — told the real cones it
    // makes the texture 4992 wide and puts eye1 at 991, Meta's layout, and the
    // owner on device: "wirkt tiefer und steht besser im Raum". Told the union
    // it still offset eye1 (-64 in a 3936 texture) and the picture was flat
    // (2026-10-18).
    if (t && strcmp(t, "x2718606324833775") == 0) return 0;
    extern int kl_untested_profile(void);
    extern kl_guest_kind kl_driver_kind(void);
    if (kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY) {
        extern const char *g_native_lib_dir;
        char path[1024];
        snprintf(path, sizeof path, "%s/libOculusXRPlugin.so", g_native_lib_dir ? g_native_lib_dir : ".");
        if (kl_can_load(path)) return 1;      // the ELF on a host, its translation in a bundle
    }
    return 0;
}

static const float *klovrp_eye_tan(int eye);
void kl_ovrp_eye_display_tangents(int eye, float tan[4]) {
    if ((unsigned)eye > 1) eye = 0;
    const float *forced = klovrp_forced_tan();
    const float *raw = forced ? forced + 4 * eye : &g_eye_tan[eye][0];
    memcpy(tan, raw, 4 * sizeof(float));
}
void kl_ovrp_eye_texture_size_for_cone(int *w, int *h) {
    int sw = 0, sh = 0;                                         // one snapshot, under the lock
    kl_ovrp_eye_texture_size(&sw, &sh);
    int ew = sw, eh = sh;
    const float *forced = klovrp_forced_tan();
    const float *raw = forced ? forced : &g_eye_tan[0][0];      // eye 0, as measured
    const float *told = klovrp_eye_tan(0);                      // ...and as told
    float wr = raw[0] + raw[1], wt = told[0] + told[1];
    float hr = raw[2] + raw[3], ht = told[2] + told[3];
    if (wr > 0.01f && hr > 0.01f && (wt < wr * 0.99f || ht < hr * 0.99f) && wt <= wr * 1.01f) {
        // A capped cone (KL_OVRP_EYE_TAN_MAX): fewer tangents at the same
        // pixels a tangent, so the pixels that are saved are saved.
        ew = (int)((float)ew * wt / wr + 0.5f);
        eh = (int)((float)eh * ht / hr + 0.5f);
        static int said;
        if (!said++)
            fprintf(stderr, "  [ovrp] eye texture narrowed with the capped cone: %dx%d -> "
                            "%dx%d (%.0f%% of the pixels, same density)\n", sw, sh,
                    ew, eh, (double)(100.0f * (float)ew * (float)eh / ((float)sw * (float)sh)));
    } else
    if (wr > 0.01f && wt > wr * 1.01f) {
        float k = sqrtf(wt / wr);
        ew = (int)((float)ew * k + 0.5f);
        eh = (int)((float)eh / k + 0.5f);
        static int said;
        if (!said++)
            fprintf(stderr, "  [ovrp] eye texture reshaped for the unified cone: %dx%d -> "
                            "%dx%d (same area, %.0f%% wider cone)\n",
                    sw, sh, ew, eh, (double)((wt / wr - 1.0f) * 100.0f));
    }
    if (w) *w = ew;
    if (h) *h = eh;
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
    // KL_OVRP_EYE_TAN_MAX=<t> — no cone is told wider than t in any direction.
    //
    // A picture is uniform in TANGENT, not in angle, so its outermost degrees
    // are its most expensive: of a Vision Pro eye's 105 degrees the 8 from 52
    // to 60 on the temporal side are a sixth of the width (tan 1.28 to 1.73 of
    // 2.73), and they land where the display's own foveation map draws at an
    // eighth of its central density. Capping the tangent gives those pixels
    // back — the eye texture is narrowed in proportion
    // (kl_ovrp_eye_texture_size_for_cone), so density is kept and cost falls —
    // at the price of a dark band at the very edge of view. The composite needs
    // no change: it places the picture as a quad of the size it was rendered
    // with, and beyond the quad is the clear colour.
    static float cap = -1.0f;
    if (cap < 0) cap = kl_env_float("KL_OVRP_EYE_TAN_MAX", 0.0f);
    if (!on && cap > 0.05f) {
        static float c[2][4];
        int cut = 0;
        for (int e = 0; e < 2; e++)
            for (int i = 0; i < 4; i++) {
                c[e][i] = src[e][i] > cap ? cap : src[e][i];
                cut |= src[e][i] > cap;
            }
        static int said;
        if (cut && !said++)
            fprintf(stderr, "  [ovrp] KL_OVRP_EYE_TAN_MAX=%.3f: eye 0 told l=%.4f r=%.4f t=%.4f "
                            "b=%.4f instead of l=%.4f r=%.4f t=%.4f b=%.4f\n", (double)cap,
                    (double)c[0][0], (double)c[0][1], (double)c[0][2], (double)c[0][3],
                    (double)src[0][0], (double)src[0][1], (double)src[0][2], (double)src[0][3]);
        return c[eye];
    }
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

void kl_ovrp_eye_told_tangents(int eye, float tan[4]) {
    memcpy(tan, klovrp_eye_tan(eye), 4 * sizeof(float));
}

// The head->eye offsets, head-local metres, and the guest's whole source of
// stereo separation — see kl_ovrp.h. Zero is the historical host behaviour and
// stays the default; the visionOS compositor pushes the display's own numbers.
static float g_eye_off[2][3];

void kl_ovrp_set_eye_offset(int eye, float x, float y, float z) {
    if ((unsigned)eye > 1) return;
    g_eye_off[eye][0] = x; g_eye_off[eye][1] = y; g_eye_off[eye][2] = z;
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

// --- the eye transform, pinned for the guest's frame ------------------------
//
// Part of one pose per guest frame (from rebelancap's fork, commit be18669 —
// see the ONE POSE PER GUEST FRAME block further down). Nothing below is
// valid, counted or read until that has frozen its first pose.
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

// How much the eye's ORIENTATION moves between pushes, in degrees: an eye
// transform that turns between the guest's two eye renders is a per-eye error
// no reprojection can undo. Printed on the one-pose census line and counted
// only while one-pose is running (g_eye_frame_valid).
static uint64_t g_eye_rot_pushes, g_eye_rot_changes;
static float    g_eye_rot_worst_deg;

// The shortest angle between two unit quaternions laid out xyzw, in degrees.
static float klovrp_quat4_degrees(const float a[4], const float b[4]) {
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * (180.0f / 3.14159265358979f);
}

void kl_ovrp_set_eye_rotation(int eye, float qx, float qy, float qz, float qw) {
    if ((unsigned)eye > 1) return;
    // A zero quaternion is not a rotation; treat it as "none" rather than
    // collapsing the eye's basis to nothing.
    if (!(qx * qx + qy * qy + qz * qz + qw * qw > 1e-6f)) return;
    if (__atomic_load_n(&g_eye_frame_valid, __ATOMIC_RELAXED)) {
        const float n[4] = { qx, qy, qz, qw };
        float d = klovrp_quat4_degrees(g_eye_rot[eye], n);
        g_eye_rot_pushes++;
        // Not the first pair of pushes, which may be identity -> the real cant
        // and would always read as "a change".
        if (g_eye_rot_pushes > 2 && d > 0.0f) {
            g_eye_rot_changes++;
            if (d > g_eye_rot_worst_deg) g_eye_rot_worst_deg = d;
        }
    }
    g_eye_rot[eye][0] = qx; g_eye_rot[eye][1] = qy;
    g_eye_rot[eye][2] = qz; g_eye_rot[eye][3] = qw;
}

static void klovrp_eye_frame_promote(void) {
    uint32_t s = __atomic_load_n(&g_eye_frame_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_eye_frame_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(g_eye_frame.off, g_eye_off, sizeof g_eye_frame.off);
    memcpy(g_eye_frame.rot, g_eye_rot, sizeof g_eye_frame.rot);
    __atomic_store_n(&g_eye_frame_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_eye_frame_valid, 1, __ATOMIC_RELEASE);
}

// Fills `off`/`rot` with the frame's eye transform and returns 1; 0 before the
// first promotion — which is always, with one-pose off — where the caller
// falls through to the live values.
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
// there is one, the live push otherwise. Returns a pointer into caller-owned
// storage or into g_eye_rot.
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
static void klovrp_eye_offset_raw(int eye, float *ox, float *oy, float *oz);
// The eyes as a Quest has them: side by side, nothing in front of or above the
// other. Vision Pro's measured offsets carry about 0.8 mm forward/back and
// 0.2 mm up/down per eye (device log: L (-0.0317, 0.0002, 0.0008) R (0.0317,
// -0.0002, -0.0008)). Invisible in a world at human scale - but LEGO
// Bricktales scales its world about 2500x, the 1.6 mm became 4 units, and its
// fade canvas, placed 2 units past the near plane in front of the camera, sat
// in front of the near plane for the right eye: the fade showed in the left
// eye only (GPU trace, draw 329, ndc z 0.980 left / 1.018 right; 2026-10-18).
// The separation stays the measured one (x); y and z are dropped. The
// composite uses these same told offsets, so it stays consistent.
// KL_OVRP_EYE_FLAT=0 hands the measured offsets on unchanged.
static void klovrp_eye_offset(int eye, float *ox, float *oy, float *oz) {
    klovrp_eye_offset_raw(eye, ox, oy, oz);
    static int flat = -1;
    if (flat < 0) flat = kl_env_on("KL_OVRP_EYE_FLAT", 1);
    if (flat) { *oy = 0.0f; *oz = 0.0f; }
}
static void klovrp_eye_offset_raw(int eye, float *ox, float *oy, float *oz) {
    static float forced = -1.0f;
    if (forced < 0.0f) {
        float v = kl_env_float("KL_OVRP_IPD", 0.0f);
        forced = (v > 0.0f && v < 0.2f) ? v : 0.0f;
    }
    if (forced > 0.0f) {
        *ox = (eye == 1 ? 0.5f : -0.5f) * forced; *oy = 0.0f; *oz = 0.0f;
        return;
    }
    // One pose per guest frame: the frame's promoted offsets where there are
    // any, so both eyes and every query inside one guest frame see ONE
    // device_from_view. Never taken for a guest that does not arm it.
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

// ovrp_GetUserEyeHeight2(float* height) — the standing eye height, metres. Unlike
// the neck-eye distance (which is safely 0), a zero here is NOT survivable under a
// FloorLevel tracking origin: the guest adds it to y=0 to place the camera, so 0
// puts the head on the floor. wanderer (UE5) polls it once per frame right before
// EndFrame4 and aborted on the unimplemented trampoline. Answer the standard
// average standing eye height; the guest's own SetTrackingOriginType decides
// whether it is applied (EyeLevel ignores it). Honours a KL_OVRP_EYE_HEIGHT
// override for tuning.
static uint64_t klovrp_GetUserEyeHeight2(float *out) {
    ovrp_hit("ovrp_GetUserEyeHeight2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    float h = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    if (!(h > 0.0f && h < 3.0f)) h = 1.6f;
    *out = h;
    return OVRP_SUCCESS;
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

// --- where an EYE-LEVEL origin is ------------------------------------------
//
// A frontend publishes poses in ITS world, and on a headset that world's y=0 is
// the floor: the head arrives a seated or standing eye height above it. That is
// what a FloorLevel guest means by a pose. An EyeLevel guest means something
// else — on a Quest the EyeLevel origin is where the head WAS when the origin
// was established, so the head starts at y=0 and a seated title authors its
// whole scene around that point. Hand such a guest the frontend's height and
// its camera is that far above everything it placed: Moss, on device, showed
// its splash (a head-relative layer) and then only the dark backdrop of a first
// screen that was 1.19 m below the view.
//
// So when the guest EXPLICITLY selects EyeLevel, the head height at that moment
// becomes the origin's height, and every pose crossing this seam is shifted by
// it: down on the way to the guest (head, hands), back up on the way to a
// compositor (the frame record, world-locked overlays, the published head), so
// the display side keeps working in the frontend's world. Height only — the
// frontend's world already starts under the user facing forward, and yaw/XZ are
// its recenter's business. Applied only WHILE the origin is EyeLevel, so a
// guest that later switches to FloorLevel gets real heights again, and never
// for a guest that merely never said (g_tracking_origin's default is also 0).
static float g_eye_origin_y;           // the frontend-world height of the origin
static int   g_eye_origin_have;        // ...once captured
static int   g_eye_origin_want;        // capture at the next head pose
static float g_head_raw_y;             // the last height the frontend published
static int   g_head_raw_seen;
static inline float klovrp_origin_y(void) {
    return (g_tracking_origin == 0 && g_eye_origin_have) ? g_eye_origin_y : 0.0f;
}
static void klovrp_eye_origin_capture(float y, const char *why) {
    g_eye_origin_y = y;
    g_eye_origin_have = 1;
    g_eye_origin_want = 0;
    fprintf(stderr, "  [ovrp] EyeLevel origin placed at the head's height, %.3f m "
                    "above the frontend's floor (%s) — poses to the guest are "
                    "lowered by that, poses back to the compositor raised\n",
            (double)y, why);
}
static void klovrp_eye_origin_request(const char *why) {
    if (g_head_raw_seen) klovrp_eye_origin_capture(g_head_raw_y, why);
    else                 g_eye_origin_want = 1;      // no head yet: take the first
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
    if (origin == 0 && !g_eye_origin_have) klovrp_eye_origin_request("the guest selected EyeLevel");
    g_tracking_origin = origin;
    return OVRP_TRUE;                       // ovrpBool
}
static uint64_t klovrp_SetTrackingOriginType2(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType2");
    ovrp_log_arg("ovrp_SetTrackingOriginType2", origin, __builtin_return_address(0));
    if (origin == 0 && !g_eye_origin_have) klovrp_eye_origin_request("the guest selected EyeLevel");
    g_tracking_origin = origin;
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
//
// One event is generated: PassthroughLayerResumed (ovrpEventType 500, data
// { int LayerId }), once a passthrough layer is first filed after the guest
// initialised passthrough — what a Quest's compositor says when the room is
// actually showing under that layer. OVRPassthroughLayer raises its
// passthroughLayerResumed from it, and a game that waits for the room before
// it takes its own world away (Synth Riders' MixedRealityManager, "the MR
// button does nothing", 2026-10-15) waits on that. Only where passthrough is
// offered (klovrp_pt_on) and KL_OVRP_PT_RESUMED is on — Synth Riders only by
// default; everyone else still never sees an event.
//
// The emulated room (KL_OVRP_SCENE_ROOM, further down) adds the scene ones:
// SpaceSetComponentStatusComplete (50), SpaceDiscoveryResultsAvailable (300)
// and SpaceDiscoveryComplete (301). Numbers and data layouts as Synth Riders'
// own OVRPlugin.cs / OVRDeserialize.cs carry them (Cpp2IL dump, 2026-10-15).
// A small FIFO, filled from whichever thread files the request and drained by
// the guest's OVRManager every frame.
#define KLOVRP_EVENT_PASSTHROUGH_LAYER_RESUMED 500u
#define KLOVRP_EV_MAX 32
#define KLOVRP_EV_DATA 4000                    // EventDataBuffer.EventData
static struct { uint32_t type; unsigned char data[KLOVRP_EV_DATA]; } g_ev_q[KLOVRP_EV_MAX];
static unsigned g_ev_head, g_ev_tail;
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_ev_data[4096];          // what PollEvent2 hands out, valid until the next poll
static void klovrp_event_push(uint32_t type, const void *data, size_t len) {
    pthread_mutex_lock(&g_ev_mu);
    if (g_ev_tail - g_ev_head < KLOVRP_EV_MAX) {
        unsigned i = g_ev_tail++ % KLOVRP_EV_MAX;
        g_ev_q[i].type = type;
        memset(g_ev_q[i].data, 0, KLOVRP_EV_DATA);
        if (data && len) memcpy(g_ev_q[i].data, data, len < KLOVRP_EV_DATA ? len : KLOVRP_EV_DATA);
    }
    pthread_mutex_unlock(&g_ev_mu);
}
static int klovrp_event_take(uint32_t *type) {
    int got = 0;
    pthread_mutex_lock(&g_ev_mu);
    if (g_ev_head != g_ev_tail) {
        unsigned i = g_ev_head++ % KLOVRP_EV_MAX;
        *type = g_ev_q[i].type;
        memset(g_ev_data, 0, sizeof g_ev_data);
        memcpy(g_ev_data, g_ev_q[i].data, KLOVRP_EV_DATA);
        got = 1;
    }
    pthread_mutex_unlock(&g_ev_mu);
    if (got) {
        static int said;
        if (said++ < 40) fprintf(stderr, "  [ovrp] event %u handed to the guest\n", *type);
    }
    return got;
}

static uint64_t klovrp_PollEvent2(uint32_t *type, void **data) {
    ovrp_hit("ovrp_PollEvent2");
    if (!type || !data) return OVRP_FAIL_INVALID_PARAM;
    if (klovrp_event_take(type)) { *data = g_ev_data; return OVRP_SUCCESS; }
    return OVRP_FAIL_OPERATION;                    // the queue is empty
}

// The v1 spelling takes the buffer by reference and fills a type field inside
// it; same empty answer, and the same refusal for a NULL. The buffer is
// { ovrpEventType EventType; uint8_t EventData[4000]; }.
static uint64_t klovrp_PollEvent(void *buf) {
    ovrp_hit("ovrp_PollEvent");
    if (!buf) return OVRP_FAIL_INVALID_PARAM;
    uint32_t type;
    if (klovrp_event_take(&type)) {
        memcpy(buf, &type, sizeof type);
        memcpy((unsigned char *)buf + 4, g_ev_data, 4000);
        return OVRP_SUCCESS;
    }
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

// The pose of a tracking origin relative to the current tracking transform.
// Real signature: (ovrpPosef* out, ovrpTrackingOrigin origin). Nothing has been
// recentered here, so the orientation is identity and x/z are 0 — but the
// origins are NOT one point. The head is posed klovrp_eye_height() above y=0
// under FloorLevel (see that function), which is exactly the statement that the
// EyeLevel origin sits that far above the floor one, and this call is how a
// guest asks for the distance.
//
// Answering (0,0,0) for every origin cost Arkham Shadow its whole picture:
// PlayerController.SetCalibratedEyeHeightToMatchTrackingOrigin asks for
// EyeLevel (0) while running FloorLevel, stores the y as OffsetTracker's
// calibratedEyeHeight, and every frame divides the live eye height by it.
// 1.6 / 0 through an AnimationCurve is NaN, which went into TrackingOffset's
// localPosition.y and from there into the player, the camera and the audio
// listener — sound, a compositor presenting frames, and nothing in them.
// Liminal, the first caller, only needed the entry point to exist.
static uint64_t klovrp_GetTrackingTransformRelativePose(float *out, int origin) {
    ovrp_hit("ovrp_GetTrackingTransformRelativePose");
    if (!out) return -1001;
    // Height of each origin above the floor; FloorLevel (1) and Stage (2) are
    // both on it. klovrp_eye_height() is 0 while the CURRENT origin is EyeLevel,
    // so the configured height is read by asking as a floor-level guest would.
    float h = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    if (!(h > 0.0f && h < 3.0f)) h = 1.6f;
    float want = origin == 0 ? h : 0.0f;
    float have = g_tracking_origin == 0 ? h : 0.0f;
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;  // orientation
    out[4] = 0.0f; out[5] = want - have; out[6] = 0.0f;          // position
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
    *out = !(klovrp_is_tracker(n) && klovrp_no_trackers());
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
} klovrp_pose;
static klovrp_pose g_head_pose = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0,   // motion_valid: nothing has been published, so there is nothing to know
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

// Optional virtual locomotion for room-scale-only titles on a host whose
// immersive safety boundary cannot be enlarged. The frontend still publishes
// the user's physical poses; this offset moves the complete tracked rig in the
// guest's world, so head and hands never separate. Richie's Plank Experience
// is the first target: its gameplay otherwise requires physically crossing
// visionOS's fixed boundary. Other titles keep their own locomotion unchanged.
static float  g_stick_move_x, g_stick_move_z;
static double g_stick_move_time;

static double klovrp_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int klovrp_stick_move_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *t = kl_driver_target_name();
        enabled = kl_env_on("KL_OVRP_STICK_MOVE",
                            t && strcmp(t, "x1642239225880682") == 0);
    }
    return enabled;
}

static void klovrp_stick_move_offset(float *px, float *pz) {
    if (!klovrp_stick_move_enabled()) return;
    *px += g_stick_move_x;
    *pz += g_stick_move_z;
}

// Integrate the LEFT stick once per frontend frame. Forward is the head's
// horizontal -Z rather than a fixed world axis, matching conventional smooth
// locomotion. A radial deadzone prevents controller drift; the remaining range
// is rescaled so full deflection still reaches the configured speed.
static void klovrp_stick_move_step(float sx, float sy) {
    if (!klovrp_stick_move_enabled()) return;
    double now = klovrp_mono_now();
    double dt = g_stick_move_time > 0 ? now - g_stick_move_time : 0;
    g_stick_move_time = now;
    if (dt <= 0 || dt > 0.1) return;

    float mag = sqrtf(sx*sx + sy*sy);
    const float dead = 0.20f;
    if (mag <= dead) return;
    float scaled = (mag - dead) / (1.0f - dead);
    if (scaled > 1.0f) scaled = 1.0f;
    sx = sx / mag * scaled;
    sy = sy / mag * scaled;

    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    float yaw = atan2f(2.0f*(h.qw*h.qy + h.qx*h.qz),
                       1.0f - 2.0f*(h.qy*h.qy + h.qz*h.qz));
    float cs = cosf(yaw), sn = sinf(yaw);
    float speed = kl_env_float("KL_OVRP_STICK_MOVE_SPEED", 1.0f);
    float seconds = (float)dt * speed;
    // local +X is right; local -Z is forward, and stick +Y means forward.
    g_stick_move_x += (sx * cs - sy * sn) * seconds;
    g_stick_move_z += (-sx * sn - sy * cs) * seconds;
}

// `when` is the instant the sample IS ABOUT, not the instant this call happens
// to be made at. 0 means the publisher has no such clock, and then the call
// time is the only answer available.
static void klovrp_hist_note(klovrp_motion_hist *h, const klovrp_pose *v,
                             double when) {
    h->have = 1;   h->t  = when > 0 ? when : klovrp_mono_now();
    h->px = v->px; h->py = v->py; h->pz = v->pz;
    h->qx = v->qx; h->qy = v->qy; h->qz = v->qz; h->qw = v->qw;
}

// Fill v's velocity from the difference against `h`, then advance `h`.
//
// The dt has to be the SAMPLES' OWN dt. The numerator is the difference between
// two poses the frontend obtained by asking for the device anchor at two
// PRESENTATION TIMES (KleptonCompositor.updatePoses(at:)) — a uniform grid, one
// panel slot apart. Differencing CLOCK_MONOTONIC across the two *calls* instead
// measures the wall-clock spacing of the frame loop's publish step, which
// carries every bit of scheduling jitter the render thread suffers. Dividing a
// uniform numerator by a jittery denominator multiplies the velocity by
// (dt_presentation / dt_call): not a constant-dt scale error but a per-frame
// noise term, and a guest that integrates the velocity to predict its render
// pose then gets a different answer every frame for the same real motion.
//
// So a publisher that KNOWS what instant its sample is about says so, and the
// difference is taken on that clock. Publishers with no such clock — the SDL
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
    return h;
}

// One pose per guest frame — the layer above this one. Defined below, because
// it reports its divergence against klovrp_head_published() and measures it
// with klovrp_quat_degrees; declared here because klovrp_head() is the first
// thing that has to consult it. `who` is the entry point, for the counters.
// Returns 0 at once for a guest that does not delimit its frames through
// OVRPlugin, and the caller takes the latch / step-ring path.
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

// The shortest angle between two orientations, in degrees. Used only to report
// how much motion the latch is absorbing.
static float klovrp_quat_degrees(const klovrp_pose *a, const klovrp_pose *b) {
    float d = a->qx * b->qx + a->qy * b->qy + a->qz * b->qz + a->qw * b->qw;
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * (180.0f / 3.14159265358979f);
}

// --- ONE POSE PER GUEST FRAME -----------------------------------------------
//
// Taken over from rebelancap's fork (commit be18669), where it is always on.
// It is the OVRPlugin half of "no XR prediction" (KL_XR_PREDICT is off by
// default on the OpenXR side): one latched sample per frame, never a newer one.
// The owner tested games with and without it on the headset; it was the
// default from 2026-10-15 behind KL_OVRP_ONE_POSE, and since that switch was
// removed there is no way back to a fresh pose per query. It applies to a guest
// that delimits its frames with ovrp_WaitToBeginFrame, and to no other
// (klovrp_one_pose_active): an OpenXR guest has its one pose per frame from
// kl_ovrp_frame_latch already.
//
// **The mechanism.** A real OVR runtime answers every pose query inside frame N
// from ONE predicted pose for frame N's display time. This runtime answers from
// live-ish sources — the frontend publishes at panel rate on its own thread,
// and the guest's queries are scattered across its frame: Update2(Physics)
// early, Update2(Render) later, twenty GetNodePoseState reads between them, the
// eye view later still. Each one can get a different answer (his measurement:
// up to 2.97 deg apart inside one frame). The pixels then embody some MIXTURE
// of those answers, while reprojection subtracts the single pose we filed — so
// the picture is displaced by whatever the difference happened to be, and the
// difference varies with scheduling from frame to frame. Alternating
// displacement of a degree or so, growing with head speed, fusing at rest.
//
// The fix is to stop having a "now" inside a guest frame at all. At the frame's
// boundary one head pose is frozen, and EVERY head-derived answer for the rest
// of that frame — both Update2 steps, every GetNodePoseState, the eye view —
// returns it. The record filed for reprojection is then the
// same pose by construction rather than by luck: klovrp_Update2 stores the
// frozen pose as its step sample and under the guest's frame index (g_fs), and
// those are the only two places klovrp_begin_frame_impl and the two re-filing
// paths (klovrp_record_indexed_sample, klovrp_record_late_sample) take a pose
// from.
//
// **What is frozen, and what is not.** Head and everything derived from it
// (the eyes) freeze; so does the eye transform itself (klovrp_eye_frame_promote
// — see there for why device_from_view has the same problem in miniature).
// Hands and controllers stay LIVE: they are not world-lock relevant, and
// pinning them would buy nothing but input latency.
//
// **Prediction.** The frozen pose is the LATCHED SAMPLE — the published pose as
// it stood at the frame boundary — not an extrapolation to
// kl_ovrp_predicted_display_time(). Constancy is the property; extrapolation is
// a second variable and a second way to be wrong.
//
// **Against what this file already had.** KL_OVRP_LATCH pins one pose per
// kl_ovrp_frame_latch() call, but an OVRPlugin guest's reads go through the
// Update2 step ring, which samples the live pose once per STEP — so Physics and
// Render disagree, and a second Render sample in the same frame differs again.
// With one-pose on, all of those return the one frozen pose; the latch and the
// step ring keep running underneath and are simply not reached for the head.

// How far apart the head poses handed out INSIDE one guest frame are — the
// largest angle between any two that ovrp_GetNodePoseState returned between two
// frame boundaries. Zero is the reading one-pose exists to produce; nonzero
// means some read reached around the frozen pose (here that can only be a read
// that NAMES another frame's index — see klovrp_step_head). Observed and
// reported only with one-pose on.
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

static klovrp_pose g_op_pose;
static uint32_t    g_op_seq;
static int         g_op_valid;
static int         g_op_index;          // the guest frame index it was frozen for
// Set once the guest has opened a frame by NAME (an Update2 carrying a usable
// frame index). From then on that is the only boundary, and kl_ovrp_frame_latch
// stops opening frames of its own — two boundaries in one guest frame would
// freeze two poses in it, which is the bug wearing the fix's clothes.
static int         g_op_index_keyed;

// --- the boundary is the guest's frame CYCLE, not its frame INDEX -----------
//
// The census below counts how many ovrp_Update2/3 calls each frozen pose
// served. A guest that calls ovrp_Update3 twice per frame (Render and Physics)
// must show exactly 2 per freeze. His measurement on Paint the Town Red VR, with
// the freeze keyed on the frame index: 1.00 poses per frame in the menu
// (smooth), 1.70 in gameplay (judder).
//
// In gameplay Unity's render thread TRAILS its sim thread, so the two Update3
// calls of one frame arrive carrying DIFFERENT frame indices — Physics with N,
// Render with N-1. Keying the freeze on the index sees a change and RE-FREEZES
// in the middle of the frame: that frame's pose reads get answered from TWO
// head poses, split at whichever instant the second Update3 landed, and that
// split point moves with thread scheduling every frame.
//
// So the guest delimits its own frame cycle instead: ovrp_WaitToBeginFrame is
// called exactly once per frame and is the frame's first instruction. It ARMS
// the boundary; the first klovrp_one_pose_open of the cycle spends the arm and
// freezes, and every later one in the same cycle reuses that pose whatever
// index it carries.
//
// A guest that never calls WaitToBeginFrame never arms, and for it one-pose
// does NOTHING — see klovrp_one_pose_active. g_op_arm_driven is the
// discriminator.
//
// KL_OVRP_POSE_BOUNDARY=cycle (default) | index. Only read for a guest that arms.
static int         g_op_armed;          // a frame cycle has opened and not yet frozen
static int         g_op_arm_driven;     // this guest delimits its own cycles
static uint64_t    g_op_freezes;        // opens that took a new pose
static uint64_t    g_op_reuses;         // opens that correctly reused the frame's
static uint64_t    g_op_arms;           // frame cycles seen

static int klovrp_pose_boundary_cycle(void) {
    static int on = -1;
    if (on < 0) {
        const char *m = kl_env_str("KL_OVRP_POSE_BOUNDARY", "cycle");
        int v = !(m && (m[0] == 'i' || m[0] == 'I'));   // "index" turns it off
        fprintf(stderr, "  [ovrp] pose boundary = %s (KL_OVRP_POSE_BOUNDARY=%s)"
                        " — the frozen head pose is renewed %s. Read"
                        " `poses/frame` on the pose-boundary census: it must"
                        " be 1.00.\n",
                v ? "guest frame CYCLE" : "guest frame INDEX", m ? m : "cycle",
                v ? "once per ovrp_WaitToBeginFrame"
                  : "whenever ovrp_Update2/3 carries a new frame index");
        on = v;
    }
    return on;
}

// Whether one-pose is in force for THIS guest: the guest has delimited a frame
// through OVRPlugin (ovrp_WaitToBeginFrame). Everything one-pose does asks
// this first, so for any other guest it changes nothing: no
// freeze at kl_ovrp_frame_latch or at ovrp_Update2/3, klovrp_one_pose_get
// answers no (latch and step ring as always), the eye transform stays live,
// nothing is counted and no census is printed.
//
// Deliberately NOT rebelancap's tree, where a guest that never arms keeps
// index keying. Walkabout is why: Unity on OpenXR, frames through xrWaitFrame —
// where the latch already gives it one pose per frame — with Meta's SDK calling
// ovrp_Update2(step 0, frameIdx 0) in mid-frame. Index 0 is an unconditional
// freeze, so each of those calls renewed the head the OpenXR queries read:
// 0.30 -> 0.35 inside one frame in the harness, where the latch alone gives one.
static int klovrp_one_pose_active(void) {
    return __atomic_load_n(&g_op_arm_driven, __ATOMIC_ACQUIRE);
}

// Called from the guest's own once-per-frame entry point. See above.
static void klovrp_one_pose_arm(void) {
    if (!__atomic_exchange_n(&g_op_arm_driven, 1, __ATOMIC_ACQ_REL))
        fprintf(stderr, "  [ovrp] one pose per guest frame IN FORCE: the guest "
                        "delimits its frames with ovrp_WaitToBeginFrame\n");
    // The intra-frame spread window is the same window, now that the guest is
    // telling us where its frames start.
    klovrp_ifs_frame();
    __atomic_store_n(&g_op_armed, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_op_arms, 1, __ATOMIC_RELAXED);
}

// Per entry point: how many answers this frame's frozen pose served, and how
// far it had already drifted from the live published pose by the time each one
// was answered. That divergence is exactly what the pixels WOULD have embodied
// differently without this.
static uint64_t g_op_served[KLOP_N];
static uint64_t g_op_fellthrough;       // answered live: before the first freeze
static float    g_op_div_worst;
static double   g_op_div_sum;
static uint64_t g_op_div_n;

// --- The frozen SERIES -------------------------------------------------------
//
// The frozen poses measured AGAINST EACH OTHER: the compositor obtains each
// published pose by asking ARKit for the device anchor at a per-slot
// presentation time, so if consecutive frames end up keyed to time bases that
// alternate, the series we freeze walks forward-back-forward through time while
// every individual pose is perfectly valid. A guest rendering that series draws
// a world that zigzags, and no warp can see it.
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
// pose and its sign flips at random.
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
    if (!klovrp_one_pose_active()) return 0;
    // The first cycle, armed but not yet opened: nothing frozen yet, so the
    // caller keeps its old path. Counted,
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
// `frame_index <= 0` is a caller with no frame number to give
// (kl_ovrp_frame_latch). It only matters under KL_OVRP_POSE_BOUNDARY=index,
// where it is an unconditional re-freeze; under the cycle boundary the arm
// decides and the index is not looked at.
static void klovrp_one_pose_open(int frame_index) {
    // Only for a guest that delimits its frames — klovrp_one_pose_active.
    if (!klovrp_one_pose_active()) return;
    // The frame CYCLE. Spending the arm is a single atomic exchange, so two
    // threads arriving together cannot both freeze: exactly one wins and the
    // other reuses.
    if (klovrp_pose_boundary_cycle()) {
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

    klovrp_pose h = klovrp_head_published();

    uint32_t s = __atomic_load_n(&g_op_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_op_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_op_pose = h;
    __atomic_store_n(&g_op_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_op_index, frame_index, __ATOMIC_RELEASE);
    __atomic_store_n(&g_op_valid, 1, __ATOMIC_RELEASE);
    if (frame_index > 0) __atomic_store_n(&g_op_index_keyed, 1, __ATOMIC_RELEASE);

    // The series this freeze just extended. Here rather than at the publish
    // seam because THIS is the sequence the guest renders: one entry per guest
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
                "(over 300 frames)\n",
                (unsigned long long)g_op_served[KLOP_UPDATE2],
                (unsigned long long)g_op_served[KLOP_STEP],
                (unsigned long long)g_op_served[KLOP_HEAD],
                (unsigned long long)g_op_served[KLOP_EYEVIEW],
                (unsigned long long)g_op_fellthrough,
                (double)g_op_div_worst, g_op_div_sum / dn,
                (unsigned long long)g_eye_rot_pushes,
                (unsigned long long)g_eye_rot_changes,
                (double)g_eye_rot_worst_deg);
        // The reading that names the artifact. `poses/frame` is freezes
        // divided by the guest's own frame cycles, and it must be 1.00: two
        // freezes in one guest frame means that frame's pose queries were
        // answered from two different heads. `update2 per freeze` is the same
        // fact from the other side: a guest that calls ovrp_Update2/3 twice a
        // frame shows 2.00 on a healthy freeze.
        {
            uint64_t fz = g_op_freezes, ru = g_op_reuses, ar = g_op_arms;
            double denom = ar ? (double)ar : (double)(fz ? fz : 1);
            fprintf(stderr,
                    "  [ovrp] pose boundary: freezes %llu, reuses %llu, guest "
                    "frame cycles %llu | poses/frame %.2f (must be 1.00) | "
                    "update2 per freeze %.2f (mode=%s)\n",
                    (unsigned long long)fz, (unsigned long long)ru,
                    (unsigned long long)ar,
                    (double)fz / denom,
                    fz ? (double)g_op_served[KLOP_UPDATE2] / (double)fz : 0.0,
                    klovrp_pose_boundary_cycle() ? "cycle" : "index");
            g_op_freezes = 0; g_op_reuses = 0; g_op_arms = 0;
        }
        klovrp_series_report();
        fprintf(stderr, "  [ovrp] intra-frame pose spread: worst %.3f deg between "
                        "two head poses ovrp_GetNodePoseState returned inside one "
                        "guest frame (over 300 frames; nonzero = reads that named "
                        "another frame's index, or straddled the boundary)\n",
                (double)g_ifs_worst_run);
        g_ifs_worst_run = 0;
        for (int i = 0; i < KLOP_N; i++) g_op_served[i] = 0;
        g_op_fellthrough = 0;
        g_op_div_worst = 0; g_op_div_sum = 0; g_op_div_n = 0;
        g_eye_rot_worst_deg = 0;   // pushes and changes run on, as totals
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
// KL_OVRP_HEAD=x,y,z[,yaw[,after,yaw2[,x2,y2,z2]]] — a head pose for a run that
// has no frontend to publish one (metres in the tracking space, yaw in degrees
// about +Y). An OpenXR guest is otherwise handed a head on the floor at the
// origin, and a game that fades the picture out when the head is inside geometry
// — Half-Life: Alyx goes orange — shows nothing of a level from there.
//
// The second pose takes over `after` seconds in. It is there because a game
// aligns its world to where the head faces when a level starts: a yaw given
// from the start is simply "forward", and looking anywhere else means turning
// afterwards.
static void klovrp_headless_pose(void) {
    static int state;                       // 0 unread, 1 none, 2 first pose, 3 second pose, 4 sweeping
    static float p[9];
    static int nparam;
    static uint64_t t0;
    // KL_OVRP_HEAD_SWEEP=<deg>,<s>[,<from>]: the head turns left and right about the KL_OVRP_HEAD
    // yaw, <deg> each way over a period of <s> seconds, from <from> seconds after the head is put
    // (a Mac run that needs the head to move once it is past the menus: the motion vectors of
    // XR_FB_space_warp, moonshot s3-framegen).
    static float sweep[3];
    if (state == 4) {
        float t = (float)((double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9) - sweep[2];
        float yaw = p[3] + (t > 0 ? sweep[0] * sinf(6.2831853f * t / sweep[1]) : 0.0f);
        float half = yaw * 0.008726646f;
        kl_ovrp_set_head_pose(p[0], p[1], p[2], 0, sinf(half), 0, cosf(half));
        return;
    }
    if (state == 1 || state == 3) return;
    if (!state) {
        // KL_OVRP_HEAD_AFTER=<s>: no head at all until then (a Mac run that must
        // get through a menu the way it does without one — HITMAN 3 VR:
        // Reloaded's pointer otherwise sits on a row and pins the selection).
        static uint64_t first;
        double wait = kl_env_float("KL_OVRP_HEAD_AFTER", 0.0f);
        if (wait > 0) {
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (!first) first = now;
            if ((double)(now - first) / 1e9 < wait) return;
        }
        const char *e = kl_env_str("KL_OVRP_HEAD", NULL);
        state = 1;
        if (!e || !*e || __atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) return;
        nparam = sscanf(e, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5],
                        &p[6], &p[7], &p[8]);
        if (nparam < 3) return;
        if (nparam < 4) p[3] = 0;
        t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        float half = p[3] * 0.008726646f;
        kl_ovrp_set_head_pose(p[0], p[1], p[2], 0, sinf(half), 0, cosf(half));
        fprintf(stderr, "  [ovrp] KL_OVRP_HEAD: no frontend, head put at (%.2f %.2f %.2f) yaw %.0f\n",
                (double)p[0], (double)p[1], (double)p[2], (double)p[3]);
        state = nparam >= 6 ? 2 : 3;
        const char *sw = kl_env_str("KL_OVRP_HEAD_SWEEP", NULL);
        if (sw && sscanf(sw, "%f,%f,%f", &sweep[0], &sweep[1], &sweep[2]) >= 2 && sweep[1] > 0) {
            state = 4;
            fprintf(stderr, "  [ovrp] KL_OVRP_HEAD_SWEEP: the head turns %.1f deg each way every %.2f s, "
                            "from %.0f s on\n", (double)sweep[0], (double)sweep[1], (double)sweep[2]);
        }
        return;
    }
    if ((double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9 < (double)p[4]) return;
    float x = nparam >= 9 ? p[6] : p[0], y = nparam >= 9 ? p[7] : p[1], z = nparam >= 9 ? p[8] : p[2];
    float half = p[5] * 0.008726646f;
    kl_ovrp_set_head_pose(x, y, z, 0, sinf(half), 0, cosf(half));
    fprintf(stderr, "  [ovrp] KL_OVRP_HEAD: head moved to (%.2f %.2f %.2f) yaw %.0f\n",
            (double)x, (double)y, (double)z, (double)p[5]);
    state = 3;
}

void kl_ovrp_frame_latch(void) {
    klovrp_headless_pose();
    // One pose per guest frame, before the latch's own enable check, and only for a
    // guest that delimits its frames through OVRPlugin (for any other this is
    // skipped whole — klovrp_one_pose_active). There this call can be the first
    // thing in a cycle: ovrp_WaitToBeginFrame arms and, where a pacer is
    // installed, calls here once the display admits the frame — so this open
    // spends the arm and the Update2/3 that follow reuse its pose. Without a
    // pacer the driver calls here before the guest has armed, and the open
    // finds no arm to spend. Under the index boundary an Update2 that named
    // its frame has taken the boundary over (g_op_index_keyed); opening here
    // as well would give that frame two poses.
    if (klovrp_one_pose_active()) {
        klovrp_ifs_frame();
        if (!__atomic_load_n(&g_op_index_keyed, __ATOMIC_ACQUIRE))
            klovrp_one_pose_open(0);
    }
    if (!klovrp_latch_enabled()) return;
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
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
    // One pose per guest frame: the frame's frozen pose, attributed to this
    // entry point rather than borrowed from klovrp_head(), so the counters say
    // which paths the guest actually asks through. A guest that never arms:
    // klovrp_head().
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

// The instant the NEXT published head pose is about, or 0 for a frontend that
// has no such clock. Written and read from the one thread that publishes.
static double g_head_pose_time;

void kl_ovrp_set_head_pose_time(double t) {
    g_head_pose_time = t;
}

void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    // DeviceAnchor does not report a velocity, so this derives one. The old
    // note here said the head's motion "is not a field the guest reads for
    // node 9 anyway" — true of an OVRPlugin guest, and node 9 is still served
    // from the same sample. It stopped being the whole story when an OpenXR
    // guest arrived: velocity there is a chained output struct on any space,
    // and zeros in it are an assertion rather than a silence.
    g_head_raw_y = py; g_head_raw_seen = 1;
    klovrp_stick_move_offset(&px, &pz);
    if (g_eye_origin_want) klovrp_eye_origin_capture(py, "first head pose after the guest selected EyeLevel");
    // Unreal's Oculus default IS EyeLevel, but Saints & Sinners only says so a
    // few seconds in — after its title splash, a world-locked quad at y=0, was
    // already up, and with no origin y=0 is the floor: the title appeared at
    // the user's feet and then jumped to eye height. For this target the
    // default is taken as stated from the first head pose (fix 15, 2026-10-11).
    // Per target on purpose: the shift is otherwise never made for a guest
    // that merely has not said. Catan VR (x3882524545155280) never says at all,
    // reads EyeLevel back and recenters: the head came in 1.1 m up and the
    // player stood far above the table (2026-10-17).
    if (!g_eye_origin_have && !g_eye_origin_want && g_tracking_origin == 0) {
        static int dflt = -1;
        if (dflt < 0) {
            const char *t = kl_driver_target_name();
            dflt = kl_env_on("KL_OVRP_EYELEVEL_DEFAULT",
                             t && (strcmp(t, "x2897337400373711") == 0 ||
                                   strcmp(t, "x3882524545155280") == 0 ||
                                   strcmp(t, "x2371757336252737") == 0));
        }
        if (dflt) klovrp_eye_origin_capture(py, "KL_OVRP_EYELEVEL_DEFAULT: the guest starts "
                                                "EyeLevel, as Unreal does");
    }
    py -= klovrp_origin_y();
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0 };
    klovrp_derive_motion(&v, &g_head_hist, g_head_pose_time);
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
                        "(0=eye,1=floor,2=stage) eye_height=%.3f  x=%.3f z=%.3f\n",
                py, yaw, pitch, kl_ovrp_tracking_origin(), kl_ovrp_eye_height(), px, pz);
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
    h.py += klovrp_origin_y();           // back into the frontend's world
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
// When each step's sample was latched (seconds, the clock
// kl_ovrp_predicted_display_time answers on), for ovrpPoseStatef.Time.
static double      g_step_time[KLOVRP_NSTEPS];
static int         g_saw_update2;

// The render step's sample, kept apart so BeginFrame can record exactly what
// the guest was told to render with rather than re-reading anything.
static klovrp_pose g_render_sample;

// Set by BeginFrame, taken by the first render-step ovrp_Update after it — see
// the note in klovrp_Update2.
// KL_OVRP_POSE_TRACE=<first frame>: print every frame-loop call for 60 frames
// from there, with thread and frame index — which sample belongs to which
// frame, in a guest's steady state rather than its first single-threaded ones.
static unsigned g_trace_frame;      // the last frame index BeginFrame saw
static int klovrp_pose_trace(unsigned frame) {
    static int from = -2;
    if (from == -2) from = kl_env_int("KL_OVRP_POSE_TRACE", -1);
    return from >= 0 && frame >= (unsigned)from && frame < (unsigned)from + 60;
}
static int g_sample_after_begin;
static void klovrp_record_late_sample(const klovrp_pose *h);

// --- samples by the guest's own FRAME INDEX ---------------------------------
//
// ovrp_Update3 and ovrp_GetNodePoseState3 both carry the frame index the sample
// is FOR, and so does ovrp_BeginFrame4. Both engines here use it consistently
// (traced, steady state): Unity samples frame N twice on its main thread, before
// and after BeginFrame(N); Unreal samples N on its game thread and then again on
// its render thread — the late update its view matrices are built from — while
// the game thread is already sampling N+1. One shared "latest render sample"
// cannot serve that: whichever thread wrote last answers every reader, and
// BeginFrame(N) files whatever is current, which for Unreal is N+1's game-thread
// sample whenever the two threads interleave that way. The picture is then
// reprojected against the next frame's pose — smooth, with the camera hopping
// back by a frame of head motion every so often (Moss, on device).
//
// So each sample is ALSO kept under its index. The frame record takes the LAST
// sample taken for its own index (at BeginFrame if it already exists, and from
// any later Update for that index while the frame is open), and a read that
// names an index is answered from that index's sample. The shared slot stays
// for everything that does not name one. `g_fs_tracks` turns this on only once
// a BeginFrame has actually found its own index among the samples, so a guest
// whose indices mean nothing keeps the older first-sample-after-Begin rule.
#define KLOVRP_FS 16
typedef struct { int idx; klovrp_pose head, hand[2]; } klovrp_fsample;
static klovrp_fsample g_fs[KLOVRP_FS];
static pthread_mutex_t g_fs_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_fs_tracks;
static __thread const klovrp_fsample *g_fs_read;   // set around an indexed read
static int klovrp_fs_get(int frame_index, klovrp_fsample *out) {
    if (frame_index <= 1) return 0;
    pthread_mutex_lock(&g_fs_mu);
    const klovrp_fsample *e = &g_fs[(unsigned)frame_index % KLOVRP_FS];
    int ok = e->idx == frame_index;
    if (ok) *out = *e;
    pthread_mutex_unlock(&g_fs_mu);
    return ok;
}
static void klovrp_record_indexed_sample(int frame_index, const klovrp_pose *h);
static int kl_env_on_once_fs(void);

static uint64_t klovrp_Update2(int step, int frame_index, double prediction) {
    ovrp_hit("ovrp_Update2");
    { static unsigned n;
      if (n < 48 || n % 600 == 0 || klovrp_pose_trace(g_trace_frame)) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        fprintf(stderr, "  [ovrp] frameloop Update2 #%u step=%d frameIdx=%d tid=%llu\n", n, step,
                frame_index, (unsigned long long)tid); } n++; }
    (void)prediction;
    int ix = klovrp_step_ix(step);

    // One pose per guest frame (both calls return at once for a guest that
    // never arms): THE FRAME BOUNDARY, for a guest that speaks OVRPlugin. Whichever step arrives
    // first in a new frame cycle (or with a new frame index, see
    // KL_OVRP_POSE_BOUNDARY) opens the frame and freezes its one pose; the
    // other step then re-uses it rather than taking a second sample of its own.
    // Everything below — the step ring, the per-index sample (g_fs) and the
    // two paths that re-file an open frame's record — then carries the frozen
    // pose, because `h` is the only pose any of them is given.
    klovrp_one_pose_open(frame_index);

    klovrp_pose h;
    if (!klovrp_one_pose_get(&h, KLOP_UPDATE2)) {
        h = klovrp_pose_read(&g_head_pose);
        if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    }
    // --- the sample the picture is DRAWN from ------------------------------
    //
    // A multithreaded Unity guest samples the render step twice a frame, both
    // on its main thread: once at the top (game logic reads it, and the render
    // thread reads the frame's eye poses from it just before ovrp_BeginFrame),
    // and once more in its before-render update, AFTER BeginFrame. The view
    // matrices it then builds its draw calls from come from that second sample.
    // Measured on Arkham Shadow with KL_OVRP_TEST_BR_YAW (below): turning the
    // head 25 degrees in the second sample alone turns the captured eye picture
    // 25 degrees.
    //
    // klovrp_begin_frame_impl recorded the FIRST one — the only sample that
    // exists at that moment — so every frame was reprojected against a pose a
    // game-logic-tick older than the one in the picture. The correction is then
    // wrong by the head's rotation across that tick: nothing with a still head,
    // and with a turning one an error that changes with every frame's logic
    // time. On device that is the world jumping aside and back during head
    // turns, at a healthy 100 Hz with every counter clean.
    //
    // So the first render-step sample taken while a frame is open replaces the
    // pose in that frame's pending record. One per frame: a pipelined main
    // thread reaches the NEXT frame's top-of-frame sample before this frame's
    // EndFrame, and that one is not this picture's. A single-threaded guest
    // (1.28's legacy VRDevice) samples before BeginFrame and never in between,
    // so nothing changes for it. KL_OVRP_RECORD_LATE=0 restores the old record.
    int after_begin = ix == 0 &&
        __atomic_exchange_n(&g_sample_after_begin, 0, __ATOMIC_ACQ_REL);
    // KL_OVRP_TEST_BR_YAW=<deg>: turn the head by that much in exactly that
    // sample and in no other — the experiment the paragraph above reports.
    // Diagnostic only.
    { static float yaw = -1.0f;
      if (yaw < 0.0f) yaw = kl_env_float("KL_OVRP_TEST_BR_YAW", 0.0f);
      if (yaw > 0.0f && after_begin) {
          float a = yaw * (3.14159265358979f / 180.0f) * 0.5f;
          float sy = sinf(a), cy = cosf(a);            // q_yaw * h
          klovrp_pose o = h;
          o.qw = cy * h.qw - sy * h.qy;
          o.qx = cy * h.qx + sy * h.qz;
          o.qy = cy * h.qy + sy * h.qw;
          o.qz = cy * h.qz - sy * h.qx;
          h = o;
      } }
    if (ix == 0 && __atomic_load_n(&g_fs_tracks, __ATOMIC_ACQUIRE))
        klovrp_record_indexed_sample(frame_index, &h);
    else if (after_begin)
        klovrp_record_late_sample(&h);
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

    if (ix == 0 && frame_index > 1) {
        pthread_mutex_lock(&g_fs_mu);
        klovrp_fsample *e = &g_fs[(unsigned)frame_index % KLOVRP_FS];
        e->idx = frame_index; e->head = h; e->hand[0] = l; e->hand[1] = r;
        pthread_mutex_unlock(&g_fs_mu);
    }
    uint32_t s = __atomic_load_n(&g_step_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_step_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_step_head[ix] = h;
    g_step_hand[ix][0] = l; g_step_hand[ix][1] = r;
    if (ix == 0) g_render_sample = h;
    __atomic_store_n(&g_step_seq, s + 2, __ATOMIC_RELEASE);
    g_step_time[ix] = kl_ovrp_predicted_display_time();
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
    int ix = klovrp_step_ix(step);
    if (ix == 0 && g_fs_read) return g_fs_read->head;
    // One pose for the frame means one pose for BOTH steps —
    // the Render and Physics samples are taken at different instants. AFTER the
    // by-index read above, not before it as in rebelancap's tree (which has no
    // such read): a read that NAMES a frame gets that frame's sample, which
    // with one-pose on is the pose that was frozen when the guest sampled for
    // that index and is the one klovrp_begin_frame_impl files for it. Answering
    // it from the current cycle's pose instead would hand a render thread still
    // drawing frame N the pose of frame N+1.
    {
        klovrp_pose op;
        if (klovrp_one_pose_get(&op, KLOP_STEP)) return op;
    }
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

static int klovrp_hand_is_wrist(int hand);
static klovrp_pose klovrp_step_hand(int step, int hand) {
    int ix = klovrp_step_ix(step);
    klovrp_pose p;
    if (ix == 0 && g_fs_read)
        p = g_fs_read->hand[hand];
    else if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        p = klovrp_seq_read(&g_step_hand[ix][hand], &g_step_seq);
    else
        p = klovrp_hand(hand);
    // Here and not in klovrp_hand: this is the OVRPlugin guest's read.
    // kl_ovrp_hand_motion is kl_openxr's and must keep the grip. Not for a
    // tracked hand's wrist, which is OVR's hand root as it stands.
    if (!klovrp_hand_is_wrist(hand)) klovrp_grip_to_touch(&p, hand);
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
        // Two for AC Nexus. Unity keeps a 4x multisampled colour and depth
        // target per stage beside each eye image, transient on a Quest and
        // real memory here (865 MB a stage at this eye size — a memoryless
        // target is refused by the GPU: "too much geometry"). With three the
        // level load peaked at 8.1 GB of an 8.2 GB limit and was killed once;
        // with two it peaks at 7.1-7.6.
        const char *t = kl_driver_target_name();
        int dflt = t && !strcmp(t, "x5812519008825194") ? 2 : KLOVRP_STAGES_DEFAULT;
        n = kl_env_int("KL_OVRP_STAGES", dflt);
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
    // ...unless the guest sampled FOR this frame index, in which case that
    // sample is the answer whatever has been written since — see g_fs.
    {
        klovrp_fsample fs;
        if (kl_env_on_once_fs() && klovrp_fs_get(guest_frame_index, &fs)) {
            h = fs.head;
            if (!__atomic_exchange_n(&g_fs_tracks, 1, __ATOMIC_ACQ_REL))
                fprintf(stderr, "  [ovrp] frame records follow the guest's frame "
                                "index: BeginFrame(%d) found a sample taken for "
                                "%d (KL_OVRP_RECORD_INDEXED=0 reverts)\n",
                        guest_frame_index, guest_frame_index);
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
    // Armed last, once the pending record is this frame's: the guest's
    // before-render sample may now replace its pose (klovrp_Update2).
    __atomic_store_n(&g_sample_after_begin, 1, __ATOMIC_RELEASE);
    return OVRP_SUCCESS;
}

static int kl_env_on_once_fs(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_RECORD_INDEXED", 1);
    return on;
}

// An Update for the frame that is OPEN right now: the guest re-sampled the frame
// it is drawing (Unity's before-render update), so the record follows it.
static void klovrp_record_indexed_sample(int frame_index, const klovrp_pose *h) {
    if (frame_index <= 1) return;
    pthread_mutex_lock(&g_frames.mu);
    kl_ovrp_render_pose *r = &g_frames.pending;
    int open = g_frames.serial && r->serial == g_frames.serial && !r->complete &&
               g_frames.pending_index == frame_index;
    float moved = 0;
    if (open) {
        klovrp_pose was = { .qx = r->qx, .qy = r->qy, .qz = r->qz, .qw = r->qw };
        moved = klovrp_quat_degrees(&was, h);
        r->px = h->px; r->py = h->py; r->pz = h->pz;
        r->qx = h->qx; r->qy = h->qy; r->qz = h->qz; r->qw = h->qw;
    }
    pthread_mutex_unlock(&g_frames.mu);
    if (!open) return;
    static float worst, sum;
    static unsigned n;
    sum += moved;
    if (moved > worst) worst = moved;
    if (++n % 600 == 0) {
        fprintf(stderr, "  [ovrp] frame record re-sampled for its own index while "
                        "open: %.2f deg mean, %.2f worst over the last 600\n",
                (double)(sum / 600), (double)worst);
        sum = 0; worst = 0;
    }
}

static void klovrp_record_late_sample(const klovrp_pose *h) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_RECORD_LATE", 1);
    if (!on) return;
    pthread_mutex_lock(&g_frames.mu);
    kl_ovrp_render_pose *r = &g_frames.pending;
    // Only a frame that is still open. `complete` is set when EndFrame files
    // the record, after which `pending` is a stale copy until the next Begin.
    int open = g_frames.serial && r->serial == g_frames.serial && !r->complete;
    float moved = 0;
    if (open) {
        klovrp_pose was = { .qx = r->qx, .qy = r->qy, .qz = r->qz, .qw = r->qw };
        moved = klovrp_quat_degrees(&was, h);
        r->px = h->px; r->py = h->py; r->pz = h->pz;
        r->qx = h->qx; r->qy = h->qy; r->qz = h->qz; r->qw = h->qw;
    }
    pthread_mutex_unlock(&g_frames.mu);
    if (!open) return;
    // How far apart the two samples were — the error every frame used to carry.
    static float worst, sum;
    static unsigned n;
    sum += moved;
    if (moved > worst) worst = moved;
    if (++n % 600 == 0 || n == 1) {
        fprintf(stderr, "  [ovrp] frame record takes the before-render sample: "
                        "%.2f deg mean, %.2f worst from the BeginFrame one over "
                        "the last %u frame(s)\n",
                (double)(sum / (n == 1 ? 1 : 600)), (double)worst, n == 1 ? 1u : 600u);
        sum = 0; worst = 0;
    }
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
    { static unsigned n; g_trace_frame = n; if (n < 48 || n % 300 == 0 || klovrp_pose_trace(n)) {
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
static void klovrp_submit_eye_color(const void *layer_submits, int count);
// ...and the census of the whole list, for the same forward-declaration reason.
static void klovrp_census_submits(const void *layer_submits, int count);
// ...and the overlay list a compositor draws on top of the eye picture.
static void klovrp_record_overlays(const void *layer_submits, int count);
static void klovrp_mirror_array_eyes(const void *layer_submits, int count);
// ...and whether the list holds a passthrough layer — the guest's own mixed
// reality, further down beside the eye-layer switch.
static void klovrp_pt_submits(const void *layer_submits, int count);
static int klovrp_pt_lut_on(void);
#define KLOVRP_PT_LUT_MAX_RES 64   // the largest colour LUT accepted (see klovrp_pt_lut_on)
static void klovrp_mirror_enq_gl(void);
static void klovrp_enq_age(void);
static int klovrp_enq_retire(void);

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
// ---------------------------------------------------------------------------
// A Vulkan frame filed when the GPU has finished it, not when the guest says it
// has (KL_VK_FRAME_ASYNC on the OVRPlugin path; default on for the titles in
// klovrp_frame_async_title below).
//
// EndFrame waited for the guest's queue to go idle before filing the frame's
// record (kl_vulkan_frame_done; why the record must not come first is written
// at that call). The wait stands on the guest's render thread, so a frame cost
// its CPU time PLUS its GPU time. LEGO Bricktales on the headset: the game's
// frame call took 9.9 ms on average with the wait (a third of them over the
// 10 ms display period) and 2.8 ms without it (headset A/B, 2026-10-18). In
// the hub that did not yet show more pictures (98.5 a second either way).
//
// Here EndFrame MARKS the queue (kl_vulkan_frame_mark), files nothing, and
// hands the record to one worker that waits for the mark, advances the Vulkan
// serial and then files the record — the order the synchronous path keeps. One
// frame is in flight: the next EndFrame waits for it first, which is also what
// keeps a three-stage guest from drawing into the stage a compositor shows.
// kl_openxr.c has the same thing for OpenXR guests (Half-Life: Alyx).
//
// Not deferred: the overlay list (klovrp_record_overlays). It is filed when the
// guest says so, as before, but no longer after its GPU work has finished.
//
// The titles it is on for, with the compositor's late wait (KL_CP_LATE_WAIT_MS
// 4.5, kl_ovrp_late_wait_default_ms) — each measured on the headset with both
// against neither in one session (KL_VK_FRAME_ASYNC_AB, 2026-10-18), averages
// of the seconds shown at 100 Hz:
//   LEGO Bricktales  96.5 -> 100.0 shown a second (first lab level)
//   Arkham Shadow    93.5 -> 99.3 (6.5 -> 0.7 repeated); the worker alone 95.0
//   Moss             94.2 -> 99.9 at every refresh (KL_GUEST_PACE_DIV=1); the
//                    worker alone 97.3. At its default, every second refresh,
//                    neither does anything (60 of 60, both ways).
// Not Demeo: the worker alone 93.0 against 92.3, and the late wait made it
// worse (86.5, twice the repeats) — its frames were nearly always in flight and
// mostly missed the wait anyway. Nothing changes at 50 Hz, where visionOS puts
// these games of its own accord after a minute or two. Not for a guest with
// two stages (AC Nexus): the worker lets it draw into the one being shown.
static int klovrp_frame_async_title(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        if (!t) return 0;               // asked before the guest is chosen: not yet known, not kept
        on = !strcmp(t, "x6521909757843713") || !strcmp(t, "batman") || kl_driver_target_is_moss();
    }
    return on;
}
double kl_ovrp_late_wait_default_ms(void) { return klovrp_frame_async_title() ? 4.5 : 0; }

static struct {
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
    int                 started, busy;
    int                 capturing;      // guest thread: end_frame_impl keeps its record here
    int                 have_rec;
    kl_ovrp_render_pose rec;
    unsigned long       frames, late;
} g_ovdefer = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, {0}, 0, 0 };

static void *klovrp_defer_thread(void *unused) {
    (void)unused;
    pthread_setname_np("Klepton OVR Frame Publish");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) {
        pthread_mutex_lock(&g_ovdefer.mu);
        while (!g_ovdefer.busy) pthread_cond_wait(&g_ovdefer.cv, &g_ovdefer.mu);
        int have = g_ovdefer.have_rec;
        kl_ovrp_render_pose rec = g_ovdefer.rec;
        pthread_mutex_unlock(&g_ovdefer.mu);

        int done = kl_vulkan_frame_wait(1000);
        kl_vulkan_frame_publish();
        if (have && (unsigned)rec.stage < KLOVRP_MAX_STAGES) {
            pthread_mutex_lock(&g_frames.mu);
            g_frames.r[rec.stage] = rec;
            g_frames.last_complete = rec.stage;
            g_frames.filed[rec.stage]++;
            pthread_mutex_unlock(&g_frames.mu);
        }

        pthread_mutex_lock(&g_ovdefer.mu);
        g_ovdefer.frames++;
        if (!done && g_ovdefer.late++ < 8)
            fprintf(stderr, "  [ovrp] a frame was filed before the GPU reported it "
                            "finished (no answer within 1 s) — %lu of %lu so far\n",
                    g_ovdefer.late, g_ovdefer.frames);
        g_ovdefer.busy = 0;
        pthread_cond_broadcast(&g_ovdefer.cv);
        pthread_mutex_unlock(&g_ovdefer.mu);
    }
    return NULL;
}

static int g_ab_nolate;                 // KL_VK_FRAME_ASYNC_AB_MODES: 1 = no late wait now

// The compositor, just before it takes the newest finished stage: if a frame
// is on its way through the worker, wait for it, but no longer than max_ms.
// 1 = it was filed in that time, 0 = none was in flight, -1 = gave up.
int kl_ovrp_wait_inflight(double max_ms) {
    if (!(max_ms > 0) || g_ab_nolate) return 0;
    pthread_mutex_lock(&g_ovdefer.mu);
    if (!g_ovdefer.busy) { pthread_mutex_unlock(&g_ovdefer.mu); return 0; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns = ts.tv_nsec + (long)(max_ms * 1e6);
    ts.tv_sec += ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    while (g_ovdefer.busy)
        if (pthread_cond_timedwait(&g_ovdefer.cv, &g_ovdefer.mu, &ts) == ETIMEDOUT) break;
    int r = g_ovdefer.busy ? -1 : 1;
    pthread_mutex_unlock(&g_ovdefer.mu);
    return r;
}

// Guest thread, at EndFrame of a Vulkan guest: 1 when this frame is to be
// filed by the worker. Waits for the frame before it first.
//
// KL_VK_FRAME_ASYNC_AB=<s> — an A/B in one session: the worker is used in
// every second span of <s> seconds, counted from the first frame and starting
// with the title's default, and every metrics sample (KL_STATS_CARD) goes to
// the log tagged with the side it was taken on. A frame in flight is waited
// for before a switch, so the two ways never overlap.
// KL_VK_FRAME_ASYNC_AB_MODES=3 — three spans in turn instead: worker off,
// worker on without the compositor's late wait (KL_CP_LATE_WAIT_MS), worker on
// with it; tagged async=off, async=on, async=late.
static int klovrp_defer_begin(void) {
    static int on = -1;
    static double ab_s, ab_t0;
    static int ab_phase = -1, ab_modes = 2;
    if (on < 0) {
        on = kl_env_on("KL_VK_FRAME_ASYNC", klovrp_frame_async_title());
        ab_s = kl_env_float("KL_VK_FRAME_ASYNC_AB", 0.0f);
        if (!(ab_s >= 2.0)) ab_s = 0;
        ab_modes = kl_env_int("KL_VK_FRAME_ASYNC_AB_MODES", 2) == 3 ? 3 : 2;
        if (ab_s > 0)
            fprintf(stderr, "  [ovrp] KL_VK_FRAME_ASYNC_AB: the frame worker is %s first and "
                            "switches every %.0f s%s\n", on ? "on" : "off", ab_s,
                    ab_modes == 3 ? " (off, on, on with the late wait)" : "");
        else if (on) fprintf(stderr, "  [ovrp] KL_VK_FRAME_ASYNC: frames are filed by a worker "
                                     "when the GPU has finished them; EndFrame no longer waits\n");
    }
    int use = on;
    if (ab_s > 0) {
        double now = (double)klovrp_mono_ns() / 1e9;
        if (ab_t0 == 0) ab_t0 = now;
        int phase = (int)((now - ab_t0) / ab_s) % ab_modes;
        use = ab_modes == 3 ? phase > 0 : phase ? !on : on;
        if (phase != ab_phase) {
            ab_phase = phase;
            int late = ab_modes == 3 && phase == 2;
            g_ab_nolate = ab_modes == 3 && !late;
            kl_metrics_set_tag(late ? "async=late" : use ? "async=on" : "async=off");
            fprintf(stderr, "  [ovrp] A/B: frame worker %s\n", late ? "ON + late wait" : use ? "ON" : "OFF");
        }
    }
    pthread_mutex_lock(&g_ovdefer.mu);
    while (g_ovdefer.busy) pthread_cond_wait(&g_ovdefer.cv, &g_ovdefer.mu);
    pthread_mutex_unlock(&g_ovdefer.mu);
    if (!use) return 0;
    if (!kl_vulkan_frame_mark()) return 0;
    g_ovdefer.have_rec = 0;
    g_ovdefer.capturing = 1;
    return 1;
}

static void klovrp_defer_submit(void) {
    pthread_mutex_lock(&g_ovdefer.mu);
    g_ovdefer.capturing = 0;
    if (!g_ovdefer.started) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        g_ovdefer.started = pthread_create(&t, &a, klovrp_defer_thread, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    int started = g_ovdefer.started;
    if (started) {
        g_ovdefer.busy = 1;
        pthread_cond_broadcast(&g_ovdefer.cv);
    }
    pthread_mutex_unlock(&g_ovdefer.mu);
    if (!started) {
        // No worker: finish here, the synchronous way.
        kl_vulkan_frame_wait(1000);
        kl_vulkan_frame_publish();
        if (g_ovdefer.have_rec && (unsigned)g_ovdefer.rec.stage < KLOVRP_MAX_STAGES) {
            pthread_mutex_lock(&g_frames.mu);
            g_frames.r[g_ovdefer.rec.stage] = g_ovdefer.rec;
            g_frames.last_complete = g_ovdefer.rec.stage;
            g_frames.filed[g_ovdefer.rec.stage]++;
            pthread_mutex_unlock(&g_frames.mu);
        }
    }
}

static uint64_t klovrp_end_frame_impl(int guest_frame_index, const int *viewports,
                                      const int *viewport_of, int named_stage) {
    // The stall watch's stamp (kl_stall.c): an OVRPlugin guest's frame is this
    // call, as an OpenXR guest's is xrEndFrame. Free unless KL_STALL_MS is set.
    { extern void kl_stall_frame(void); kl_stall_frame(); }
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
    if (named_stage >= 0) g_frames.last_named = named_stage;
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
        if (g_ovdefer.capturing) {
            // KL_VK_FRAME_ASYNC: the worker files it once the GPU is done.
            g_ovdefer.rec = g_frames.pending;
            g_ovdefer.have_rec = 1;
        } else {
            g_frames.r[stage] = g_frames.pending;
            g_frames.last_complete = stage;
            g_frames.filed[stage]++;
        }
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
    // Gated on the guest never having swapped rather than on a version test:
    // 1.28's legacy VRDevice calls BOTH this and eglSwapBuffers, and presenting
    // twice a frame would double every capture and hand a frontend two frames
    // per frame. The swap count answers "is the swap this guest's presentation
    // signal?" directly, which is the actual question.
    //
    // "Never" was too strong: Saints & Sinners swaps a few dozen times for its
    // pre-VR splash and then never again, so this block stopped running once
    // VR began — the GPU fence froze at the splash's last swap (fence=37 for a
    // whole session), the compositor never again waited for the guest's GPU
    // work, and it sampled each eye stage while that frame was still being
    // drawn over the one three frames older: a ghost beside every object while
    // the head turns, absent from any screenshot. The question is whether a
    // swap presented THIS frame, so it is asked per frame.
    //
    // Asked that way for Unreal guests, which is where it was seen and sent in
    // (fix 15, and again in fix 16 for Tales from the Galaxy's Edge,
    // 2026-10-11). Every other guest keeps the test it was brought up with: a
    // guest whose swap runs out of step with its EndFrame would present twice
    // in some frames under the new one, and none of those has been looked at.
    static unsigned long swaps_at_last_end;
    unsigned long swaps_now = kl_egl_swap_count();
    int swapped_this_frame = swaps_now != swaps_at_last_end;
    swaps_at_last_end = swaps_now;
    static int per_frame = -1;
    if (per_frame < 0) {
        extern kl_guest_kind kl_driver_kind(void);
        per_frame = kl_env_on("KL_OVRP_SWAP_PER_FRAME", kl_driver_kind() == KL_GUEST_UE4);
    }
    if (per_frame ? !swapped_this_frame : swaps_now == 0) {
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
    klovrp_enq_age();
    klovrp_mirror_enq_gl();
// No layer list on this path — 1.28's legacy VRDevice hands its eye
    // textures down through ovrp_SetupEyeTexture2 and never describes a layer,
    // so it can only ever have rendered into the whole thing.
    return klovrp_end_frame_impl(guest_frame_index, NULL, NULL, -1);
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
// The guest's submitted-frame counter — see KL_OVRP_POKE and kl_vulkan_capture_eyes.
static unsigned g_vk_frame;

void kl_ovrp_set_eyeless_black(int on);            // defined with the eye-layer switch, below
static volatile unsigned g_loadvid_frames_since;   // defined with the loading video, below
// EndFrames since the guest last called ovrp_EnqueueSubmitLayer — see
// klovrp_enq_owns_overlays.
static volatile unsigned g_enq_frames_since = 1000000u;
static uint64_t klovrp_EndFrame4(int guest_frame_index, const void *layer_submits,
                                 int layer_submit_count, void *sync) {
    ovrp_hit("ovrp_EndFrame4");
    if (g_loadvid_frames_since < 1000000u) g_loadvid_frames_since++;
    if (klovrp_enq_retire()) klovrp_enq_age();
    else if (g_enq_frames_since < 1000000u) g_enq_frames_since++;
    { static unsigned n; if (n < 48 || n % 300 == 0 || klovrp_pose_trace(n)) {
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
    // The guest's own answer to "which stage did I just draw?", at submit+0x04.
    // -1 when there is no eye layer in the list to read it from, which is a
    // different thing from stage 0 and has to stay distinguishable — the frame
    // record's fallback ladder tests it (klovrp_end_frame_impl).
    int named = klovrp_submit_stage(layer_submits, layer_submit_count);
    // KL_OVRP_EYELESS_FRAMES=0|1 — a submit list with no eye layer turns the eye
    // pass off until one comes back (kl_ovrp_eye_layer_external, the switch the
    // OpenXR path already drives), so the composite shows only the frame's
    // quads over black, as a Quest does. Left on, the compositor keeps drawing
    // the eye textures' last contents — for Skydance's BEHEMOTH (UE5), whose
    // splash and level-loading screens submit quads alone, that was eye storage
    // nothing had drawn yet, a magenta flicker behind every loading screen
    // (2026-10-11, KL_OVRP_LAYERS: "submit 1/1: layer 2 (Quad)").
    // Asgard's Wrath 2 (UE4) is the same thing with older pictures: while a
    // level loads its frames are the loading quads alone, and behind them the
    // compositor went round the three eye stages by frame counter ("eye stage
    // NOT observed") — three stale pictures in turn, not the same one for both
    // eyes ("the pictures sent to the eye are broken and the left eye sees
    // something else than the right", 2026-10-11; the log has `layers=1` with
    // layer 2, a 350x175 m quad at 200 m, from the first frame on).
    // Alien: Rogue Incursion (UE5, Vulkan) again: after START GAME its frames
    // are one 7x5.25 m loading quad 10 m out and nothing else
    // (KL_OVRP_LAYERS, 2026-10-16: "submit 1/1: layer 4 (Quad)"), and the eyes
    // differed while a level loaded. On Vulkan no stage is ever observed, so
    // behind that quad the stages went round by frame counter, under the last
    // eye viewport — and its dynamic resolution had left the three stages at
    // different sizes (2271x2400 in one, 1589x1680 in the others).
    // Into The Radius 2 (x6605328856182465, UE 5.5, Vulkan) too: one frame
    // with its eye layer, then three quads alone (layers 2-4, a 4096x4096 at
    // 600 m and two 256x256 at 150 m) from the next frame on while it loads,
    // and the picture flickered (2026-10-20).
    //   default 1 for x5767635983306831, wrath2, x24882487241367228 and
    //   x6605328856182465, else 0 (every other OVRPlugin guest is left with the
    //   eye pass always on, as before)
    static int eyeless = -1;
    if (eyeless < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        eyeless = kl_env_on("KL_OVRP_EYELESS_FRAMES",
                            t && (!strcmp(t, "x5767635983306831") || !strcmp(t, "wrath2") ||
                                  !strcmp(t, "x24882487241367228") ||
                                  !strcmp(t, "x6605328856182465")));
    }
    if (eyeless && layer_submits && layer_submit_count > 0) {
        kl_ovrp_eye_layer_external(named >= 0);
        kl_ovrp_set_eyeless_black(named < 0);
    }
    klovrp_submit_eye_color(layer_submits, layer_submit_count);
    klovrp_pt_submits(layer_submits, layer_submit_count);

    // BONELAB / the Vulkan path: this call IS the guest's assertion that it has
    // SUBMITTED the eye textures' drawing, so it is where they can be read back
    // and where the GPU is waited for.
    //
    // **Before the record is filed, not after.** klovrp_end_frame_impl sets
    // last_complete and files the pose, and the compositor's next pass — on its
    // own thread, up to a display period later at most — takes both and samples
    // that stage's MTLTexture. On the GL path an MTLSharedEvent orders that pass
    // behind the guest's rendering; a Vulkan guest has no such event (the
    // compositor logs fence=0), so the ONLY ordering is that the stage is not
    // announced until its picture exists. Filing first announced a stage the
    // GPU had not reached yet: for the length of the guest's GPU frame the
    // compositor reprojected by frame N's pose a texture still holding the
    // picture of frame N-stages. Standing still the two are the same picture.
    // Turning the head they are several frames of rotation apart, so the world
    // is drawn in the wrong place for one display frame and snaps back —
    // Arkham Shadow on device, "flicker when I look around, walking is smooth",
    // worst exactly when the game is GPU-bound, since that is when the window
    // is longest.
    //
    // g_vk_frame is defined down with KL_OVRP_POKE, which reads it: a poke
    // aimed at `f9900` and the capture named `vk_f09900_*.png` have to be the
    // same frame.
    //
    // The stage comes out of the submit rather than from
    // kl_ovrp_last_complete_stage(), and that is not a shortcut — the latter is
    // derived from GL draw observation (kl_glfb_render_stages), which sees
    // nothing at all on a Vulkan guest.
    int deferred = 0;
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
        uint64_t t0 = klovrp_mono_ns();
        // KL_VK_FRAME_ASYNC: the wait below is then for the frame BEFORE this
        // one, and this frame's record goes to the worker.
        deferred = klovrp_defer_begin();
        if (!deferred) kl_vulkan_frame_done(named < 0 ? 0 : named);
        // How long the GPU still had to go when the guest said "done" — the
        // width of the window described above, and the number that says
        // whether the guest is GPU-bound.
        static double sum_ms, worst_ms;
        static unsigned n;
        double ms = (double)(klovrp_mono_ns() - t0) / 1e6;
        klovrp_adapt_note(ms);
        sum_ms += ms;
        if (ms > worst_ms) worst_ms = ms;
        if (++n % 600 == 0) {
            fprintf(stderr, "  [ovrp] frame end waited for the GPU: %.1f ms mean, "
                            "%.1f worst over the last 600 frames (stage filed "
                            "after the wait)\n", sum_ms / 600.0, worst_ms);
            sum_ms = 0; worst_ms = 0;
        }
    }

    // The list of everything that is NOT the eye layer, for the compositors.
    // Before this, all of it was dropped here. After the wait for the same
    // reason the record is: these name stages of their own.
    klovrp_record_overlays(layer_submits, layer_submit_count);
    // GL array eyes reach the compositor by copy, and the copy has to land
    // before the record announces the stage (the same ordering argument as the
    // Vulkan branch above: nothing else puts the compositor behind it).
    klovrp_mirror_array_eyes(layer_submits, layer_submit_count);
    // KL_OVRP_GL_EYE_FENCE=0|1 — fence a GL guest's eye drawing here, on its
    // submitting thread, before the record announces the stage. For a guest
    // that renders straight into its eye textures and stops swapping once
    // gameplay starts: the swap-driven GPU fence then stays where it was while
    // the records advance, and the compositor can take a stage's old picture
    // under its new pose — the head jitters on a steady display cadence.
    // Gun Club VR (x2285803881494677, build 410): jitter at reduced resolution
    // and with foveation off, steady head movement with this fence (fix 44,
    // 2026-10-15, its sender's headset; not run here).
    //   default 1 for x2285803881494677, else 0
    static int gl_eye_fence = -1;
    if (gl_eye_fence < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        gl_eye_fence = kl_env_on("KL_OVRP_GL_EYE_FENCE", t && !strcmp(t, "x2285803881494677"));
    }
    if (gl_eye_fence && named >= 0 && !kl_vulkan_guest_active()) {
        kl_glfb_frame_fence();
        static int said;
        if (!said++)
            fprintf(stderr, "  [ovrp] GL eyes fenced before their frame record is "
                            "published (KL_OVRP_GL_EYE_FENCE)\n");
    }
    int vp[8], of[2] = { 0, 0 };
    int have = klovrp_submit_viewports(layer_submits, layer_submit_count, vp, of);
    uint64_t rv = klovrp_end_frame_impl(guest_frame_index, have ? vp : NULL,
                                        have ? of : NULL, named);
    if (deferred) klovrp_defer_submit();
    return rv;
}
int kl_ovrp_stage_render_pose(int stage, kl_ovrp_render_pose *out) {
    if ((unsigned)stage >= KLOVRP_MAX_STAGES || !out) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int have = g_frames.r[stage].serial != 0;
    if (have) *out = g_frames.r[stage];
    pthread_mutex_unlock(&g_frames.mu);
    if (have) out->py += klovrp_origin_y();   // the compositor works in the frontend's world
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
    r->submit_ns = klovrp_mono_ns();
    g_frames.pending_index = (int)s;
    pthread_mutex_unlock(&g_frames.mu);
}

void kl_ovrp_frame_end_external(int stage, const float *pose7, const float *tan8) {
    kl_ovrp_render_pose rec;
    if (kl_ovrp_frame_end_prepare(stage, pose7, tan8, &rec)) kl_ovrp_frame_end_commit(&rec);
}

// The same record in two halves, for a frame whose picture is finished by the
// GPU some time AFTER the guest has said it is done (kl_openxr's
// KL_VK_FRAME_ASYNC). `prepare` runs on the guest's thread at xrEndFrame and
// closes the record against the pose that frame was handed — which has to be
// read then, because the next xrWaitFrame overwrites it. `commit` files it where
// a compositor reads, and may come from another thread once the picture is
// complete. Called back to back they are kl_ovrp_frame_end_external.
void kl_ovrp_frame_end_commit(const kl_ovrp_render_pose *rec) {
    if (!rec || (unsigned)rec->stage >= KLOVRP_MAX_STAGES) return;
    pthread_mutex_lock(&g_frames.mu);
    g_frames.r[rec->stage] = *rec;
    g_frames.last_complete = rec->stage;
    g_frames.filed[rec->stage]++;
    pthread_mutex_unlock(&g_frames.mu);
}

int kl_ovrp_frame_end_prepare(int stage, const float *pose7, const float *tan8,
                              kl_ovrp_render_pose *out) {
    int filed = 0;
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
        return 0;
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
        if (out) *out = g_frames.pending;
        filed = 1;
    }
    pthread_mutex_unlock(&g_frames.mu);
    return filed;
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

// KL_GRAB_TRACE=1 (diagnostic): while a hand holds any button, its buttons,
// trigger and grip values and its pose as published, about 30 times a second,
// plus every jump of 3 cm or more between two consecutive publishes whatever
// is held. Built for LEGO Bricktales' "a brick lifted slowly drops" — it showed
// the trigger bit arriving at ~0 travel, which settled it (KleptonControllers'
// triggerClickAt).
static void klovrp_grab_trace(int hand, const klovrp_pose *v) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GRAB_TRACE", 0);
    if (!on || (unsigned)hand > 1) return;
    static float lp[2][3];
    static int have[2];
    static uint64_t lt[2];
    static unsigned lines;
    if (lines > 40000) return;
    uint64_t now = klovrp_mono_ns();
    float dx = v->px - lp[hand][0], dy = v->py - lp[hand][1], dz = v->pz - lp[hand][2];
    float step = sqrtf(dx * dx + dy * dy + dz * dz);
    int held = g_input[hand].buttons != 0;
    if ((have[hand] && step >= 0.03f) || (held && now - lt[hand] >= 33000000ull)) {
        lines++;
        lt[hand] = now;
        fprintf(stderr, "  [grab] %.3f hand %d btn %#x trig %.2f grip %.2f pos %.3f %.3f %.3f"
                        " vel %.2f %.2f %.2f step %.1f mm%s%s\n", (double)now / 1e9, hand,
                g_input[hand].buttons, (double)g_input[hand].index_trigger,
                (double)g_input[hand].hand_trigger,
                (double)v->px, (double)v->py, (double)v->pz,
                (double)v->vx, (double)v->vy, (double)v->vz, (double)step * 1000.0,
                v->motion_valid ? "" : " (vel derived)",
                have[hand] && step >= 0.03f ? " JUMP" : "");
    }
    lp[hand][0] = v->px; lp[hand][1] = v->py; lp[hand][2] = v->pz;
    have[hand] = 1;
}

// ---- hand tracking: the user's own fingers -------------------------------------
//
// OVRPlugin's hand tracking (OVRP_1_44_0: ovrp_GetHandState, ovrp_GetSkeleton,
// ovrp_GetHandTrackingEnabled) answered from ARKit's hand joints. The frontend
// publishes the 27 joints of each tracked hand once a frame
// (kl_ovrp_set_hand_joints), already predicted to the frame's presentation
// time; kl_hands.c turns them into OVR's 24-bone skeleton there and then, so
// the guest's reads only copy.
//
// Off unless asked for: KL_OVRP_HANDS=1, on by default for the titles below.
// Every other guest keeps the answers it had (tracking off, the queries
// refused), which matters because a game that sees hands may switch its whole
// input to them.
//
//   Elixir (x3793077684043441): a hands-only game; with tracking off it stops at
//   a screen asking for it to be turned on.
//   Angry Birds VR: Isle of Pigs (x2718606324833775): controllers first, like on
//   a Quest — while a Sense controller is tracked it is the input; without one
//   the hands take over.
//
//   Space Atlas VR (x8016534201745429): loads Left/RightOVRHand and the
//   avatar hand-tracking contexts; offer the existing skeleton implementation.
//
// Hands are the input — ConnectedControllers/ActiveController say Hands (0x60)
// and the hand nodes are the wrist — unless a Sense controller is in a hand.
// While one is, the frontend says so (state 2) and everything is the Touch pair
// it was before.

static int klovrp_hands_title(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        if (!t) return 0;               // asked before the guest is chosen: not kept
        on = kl_env_on("KL_OVRP_HANDS", !strcmp(t, "x3793077684043441") ||
                                        !strcmp(t, "x2718606324833775") ||
                                        !strcmp(t, "x8016534201745429"));
        if (on) fprintf(stderr, "  [hands] hand tracking for %s on (KL_OVRP_HANDS=0 turns it off)\n", t);
    }
    return on;
}
int kl_ovrp_hand_tracking_on(void) { return klovrp_hands_title(); }

static pthread_mutex_t g_hj_lock = PTHREAD_MUTEX_INITIALIZER;
static kl_ovr_hand g_hj_ovr[2];             // under g_hj_lock: what the guest reads
static kl_hand_xf  g_hj_raw[2][KLH_JOINTS];  // ...and the joints it came from (pointer pose)
static int g_hj_state[2];                   // 0 not seen, 1 tracked, 2 a controller in it
static double g_hj_time[2];
static kl_hands_ctx g_hj_ctx[2];            // the publisher's only
static int g_hj_owner[2];                   // the hand node is the wrist (atomic)
static unsigned g_hj_held[2], g_hj_failed[2], g_hj_jumps[2];   // publisher's counts, for the log
static kl_hand_xf g_hj_ptr[2] = { { { 0, 0, 0 }, { 0, 0, 0, 1 } }, { { 0, 0, 0 }, { 0, 0, 0, 1 } } };
static unsigned g_hj_reads[2], g_hj_reads_lost[2];  // guest's reads (any thread; approximate)


int kl_ovrp_hand_guide(int hand, float *joints, float *pinch, float *palm_up) {
    if (hand < 0 || hand > 1 || !joints || !pinch || !palm_up) return 0;
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x8016534201745429")) return 0;
    pthread_mutex_lock(&g_hj_lock);
    int tracked = g_hj_ovr[hand].tracked;
    kl_hand_xf root = g_hj_ovr[hand].root;
    for (int i = 0; i < KLH_JOINTS; i++)
        memcpy(joints + i * 3, g_hj_raw[hand][i].p, 3 * sizeof(float));
    *pinch = g_hj_ovr[hand].pinch[1];
    pthread_mutex_unlock(&g_hj_lock);
    if (!tracked) return 0;
    // The left OVR wrist's +Y points out of the palm (kl_hands.h).
    // This is visual feedback for the posture, not a synthetic menu press.
    klovrp_pose pose = { .qx = root.q[0], .qy = root.q[1], .qz = root.q[2], .qw = root.q[3] };
    float nx, ny, nz;
    klovrp_qrot(&pose, 0, 1, 0, &nx, &ny, &nz);
    *palm_up = ny;
    float origin_y = klovrp_origin_y();
    for (int i = 0; i < KLH_JOINTS; i++) joints[i * 3 + 1] += origin_y;
    return 1;
}

// KL_OVRP_HANDS_BESIDE=1: the controllers stay the input and the hands are only
// offered beside them — connected while ARKit sees them, their state and
// skeleton answered — for a game that switches to hands with an option of its
// own. Off by default: Angry Birds VR turned out to have no such option, and
// switches by itself once hands are connected.
static int klovrp_hands_beside(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        if (!t) return 0;
        on = klovrp_hands_title() && kl_env_on("KL_OVRP_HANDS_BESIDE", 0);
        if (on) fprintf(stderr, "  [hands] controllers stay the input, hands are offered beside them "
                                "(KL_OVRP_HANDS_BESIDE=0 makes the hands the input when no controller is held)\n");
    }
    return on;
}

// The hand bits to add to the connected controllers beside a Touch pair: each
// hand ARKit is tracking right now.
static uint32_t klovrp_hands_beside_bits(void) {
    if (!klovrp_hands_beside()) return 0;
    pthread_mutex_lock(&g_hj_lock);
    uint32_t b = (g_hj_ovr[0].tracked ? 0x20u : 0) | (g_hj_ovr[1].tracked ? 0x40u : 0);
    pthread_mutex_unlock(&g_hj_lock);
    return b;
}

// Hands are the input: hand tracking is on for this guest, the controllers are
// not to stay the input, and neither hand holds a controller.
static int klovrp_hands_mode(void) {
    if (!klovrp_hands_title() || klovrp_hands_beside()) return 0;
    return __atomic_load_n(&g_hj_state[0], __ATOMIC_ACQUIRE) != 2 &&
           __atomic_load_n(&g_hj_state[1], __ATOMIC_ACQUIRE) != 2;
}

static void klovrp_hands_log_first(int hand, const kl_hand_xf *j, const kl_ovr_hand *o) {
    // Lengths between the joints ARKit gives, so the joint mapping is checkable
    // against a hand: wrist to thumb base ~4.5 cm, to the middle knuckle ~9.5.
    #define KLH_D(a, b) sqrtf((j[a].p[0] - j[b].p[0]) * (j[a].p[0] - j[b].p[0]) + \
                              (j[a].p[1] - j[b].p[1]) * (j[a].p[1] - j[b].p[1]) + \
                              (j[a].p[2] - j[b].p[2]) * (j[a].p[2] - j[b].p[2]))
    fprintf(stderr, "  [hands] %s hand first seen: wrist-thumb base %.1f cm, wrist-middle knuckle %.1f, "
                    "middle finger %.1f+%.1f+%.1f, thumb %.1f+%.1f+%.1f, scale %.2f\n",
            hand ? "right" : "left",
            100 * KLH_D(KLH_WRIST, KLH_THUMB_KNUCKLE), 100 * KLH_D(KLH_WRIST, KLH_MIDDLE_KNUCKLE),
            100 * KLH_D(KLH_MIDDLE_KNUCKLE, KLH_MIDDLE_IB), 100 * KLH_D(KLH_MIDDLE_IB, KLH_MIDDLE_IT),
            100 * KLH_D(KLH_MIDDLE_IT, KLH_MIDDLE_TIP), 100 * KLH_D(KLH_THUMB_KNUCKLE, KLH_THUMB_IB),
            100 * KLH_D(KLH_THUMB_IB, KLH_THUMB_IT), 100 * KLH_D(KLH_THUMB_IT, KLH_THUMB_TIP), o->scale);
    #undef KLH_D
}

// Each finger's bend as OVR reports it: the angles of its bones' turns about
// their Z axis, summed (0 flat, ~200 a fist), and the twist about X of the
// first bone. One line per hand every few seconds while tracked.
static void klovrp_hands_log_pose(int hand, const kl_ovr_hand *o, uint32_t tracked) {
    static const int first[5] = { KLO_THUMB1, KLO_INDEX1, KLO_MIDDLE1, KLO_RING1, KLO_PINKY1 };
    float bend[5];
    for (int f = 0; f < 5; f++) {
        bend[f] = 0;
        for (int b = first[f]; b < first[f] + 3; b++) {
            const float *q = o->rot[b];
            bend[f] += 2 * atan2f(q[2], q[3]) * 57.29578f;
        }
    }
    fprintf(stderr, "  [hands] %s: bend thumb %.0f index %.0f middle %.0f ring %.0f little %.0f deg, "
                    "pinch %.2f %.2f %.2f %.2f bits %#x, wrist %.2f %.2f %.2f, joints %d/27, "
                    "fingers sure %d%d%d%d%d\n",
            hand ? "R" : "L", bend[0], bend[1], bend[2], bend[3], bend[4],
            o->pinch[1], o->pinch[2], o->pinch[3], o->pinch[4], o->pinch_bits,
            o->root.p[0], o->root.p[1], o->root.p[2], __builtin_popcount(tracked),
            o->finger_high[0], o->finger_high[1], o->finger_high[2], o->finger_high[3],
            o->finger_high[4]);
}

void kl_ovrp_set_hand_joints(int hand, int state, const float *xf, uint32_t tracked) {
    if ((unsigned)hand > 1 || !klovrp_hands_title()) return;
    static int forced = -2;
    if (forced == -2) {
        // KL_OVRP_HANDS_AXIS=+x|-x|+y|-y|+z|-z: ARKit's joint axis that points
        // out of the back of a finger, instead of finding it on a flat hand.
        const char *a = getenv("KL_OVRP_HANDS_AXIS");
        forced = -1;
        for (int i = 0; a && i < 6; i++) if (!strcmp(a, kl_hands_axis_name(i))) forced = i;
        kl_hands_init(&g_hj_ctx[0]);
        kl_hands_init(&g_hj_ctx[1]);
        if (forced >= 0) {
            kl_hands_force_axis(&g_hj_ctx[0], forced);
            kl_hands_force_axis(&g_hj_ctx[1], forced);
        }
    }
    // KL_OVRP_HANDS_ONLY=1: hands are the input even with controllers paired —
    // a controller in a hand is that hand not seen (ARKit loses a hand that
    // holds one anyway). On by default for the hands-only titles: Elixir.
    static int only = -1;
    if (only < 0) {
        const char *t = kl_driver_target_name();
        only = kl_env_on("KL_OVRP_HANDS_ONLY", t && !strcmp(t, "x3793077684043441"));
        if (only) fprintf(stderr, "  [hands] hands only: controllers are not offered to this game\n");
    }
    if (only && state == 2) state = 0;
    int was = __atomic_load_n(&g_hj_state[hand], __ATOMIC_ACQUIRE);
    // A hand ARKit loses for a few frames stays where it was, tracked, for up
    // to KL_OVRP_HANDS_HOLD_MS (default 250): a game that hides a hand the
    // moment it is not tracked otherwise blinks it out every second or two.
    static double hold_s = -1;
    if (hold_s < 0) hold_s = kl_env_int("KL_OVRP_HANDS_HOLD_MS", 250) / 1000.0;
    static double last_ok[2];
    double now_s = klovrp_mono_now();
    if (state == 0 && was == 1 && last_ok[hand] > 0 && now_s - last_ok[hand] < hold_s) {
        g_hj_held[hand]++;
        return;
    }
    if (state != 1 || !xf) {
        __atomic_store_n(&g_hj_owner[hand], 0, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_hj_lock);
        g_hj_ovr[hand].tracked = 0;
        g_hj_ovr[hand].pinch_bits = 0;
        pthread_mutex_unlock(&g_hj_lock);
        __atomic_store_n(&g_hj_state[hand], state == 2 ? 2 : 0, __ATOMIC_RELEASE);
        if (was != state && (was == 2 || state == 2))
            fprintf(stderr, "  [hands] %s hand: %s\n", hand ? "right" : "left",
                    state == 2 ? "holds a controller — Touch until it is put down"
                               : "no controller any more — hands are the input");
        return;
    }
    // Into the guest's space exactly as kl_ovrp_set_hand_motion does a pose.
    kl_hand_xf j[KLH_JOINTS];
    for (int i = 0; i < KLH_JOINTS; i++) {
        memcpy(j[i].p, xf + 7 * i, sizeof j[i].p);
        memcpy(j[i].q, xf + 7 * i + 3, sizeof j[i].q);
        klovrp_stick_move_offset(&j[i].p[0], &j[i].p[2]);
        j[i].p[1] -= klovrp_origin_y();
    }
    kl_ovr_hand o;
    int ok = kl_hands_to_ovr(hand, j, tracked, &g_hj_ctx[hand], &o);
    if (ok) {
        // A jump no hand makes: more than 12 cm from one published frame to the
        // next (several metres a second). Hands crossing or covering each other
        // make ARKit swap or misplace them for a few frames, and the aim then
        // flicks across the room (Angry Birds, 2026-10-24). The hand stays where
        // it was until the new place has held for 8 frames (~80 ms).
        static int jump_frames[2];
        pthread_mutex_lock(&g_hj_lock);
        kl_ovr_hand prev = g_hj_ovr[hand];
        pthread_mutex_unlock(&g_hj_lock);
        if (prev.tracked) {
            float dx = o.root.p[0] - prev.root.p[0], dy = o.root.p[1] - prev.root.p[1],
                  dz = o.root.p[2] - prev.root.p[2];
            if (dx * dx + dy * dy + dz * dz > 0.12f * 0.12f && jump_frames[hand] < 8) {
                jump_frames[hand]++;
                g_hj_jumps[hand]++;
                __atomic_store_n(&g_hj_state[hand], 1, __ATOMIC_RELEASE);
                return;
            }
        }
        jump_frames[hand] = 0;
        last_ok[hand] = now_s;
        // KL_OVRP_HANDS_CURL=<gain>: the four fingers' bend times this (at most
        // 100 degrees a joint). Tried as 1.3 for Angry Birds, whose slingshot
        // grab took only a very tight fist: the fingers then hooked into claws
        // even on a relaxed hand and it played worse (2026-10-24). Default 1.
        static float curl = -1;
        if (curl < 0) curl = kl_env_float("KL_OVRP_HANDS_CURL", 1.0f);
        kl_hands_scale_bend(hand, &o, curl, 100);
        // KL_OVRP_HANDS_FIST=<deg>: a finger bent more than this in all (its
        // three joints) is bent further toward a full fist (170 -> 250 deg), a
        // relaxed finger not at all. For a game whose grab wants a Quest-tight
        // fist from ARKit's looser reading (Angry Birds' slingshot: Meta's
        // FingerPalmGrabAPI). Default 110 there, 0 (off) elsewhere; once engaged
        // it holds down to 30 degrees less.
        static float fist = -1;
        if (fist < 0) {
            const char *t = kl_driver_target_name();
            fist = kl_env_float("KL_OVRP_HANDS_FIST", t && !strcmp(t, "x2718606324833775") ? 110.0f : 0.0f);
        }
        if (fist > 0) kl_hands_fist_assist(hand, &g_hj_ctx[hand], &o, fist, 250, 170);
        // The pointer ray, from an estimated shoulder through the hand, smoothed
        // by speed: still when the hand is still, prompt when it moves.
        klovrp_pose h = klovrp_pose_read(&g_head_pose);
        float fx, fy, fz;
        klovrp_qrot(&h, 0, 0, -1, &fx, &fy, &fz);
        float l = sqrtf(fx * fx + fz * fz);
        if (l < 1e-3f) { fx = 0; fz = -1; l = 1; }
        fx /= l; fz /= l;
        float side = hand ? 0.17f : -0.17f;
        float sh[3] = { h.px - fz * side - fx * 0.05f, h.py - 0.25f, h.pz + fx * side - fz * 0.05f };
        kl_hand_xf ptr;
        kl_hands_pointer(j, sh, &ptr);
        static kl_hand_xf sm[2];
        static int have_sm[2];
        if (have_sm[hand]) {
            float mx = ptr.p[0] - sm[hand].p[0], my = ptr.p[1] - sm[hand].p[1], mz = ptr.p[2] - sm[hand].p[2];
            float move = sqrtf(mx * mx + my * my + mz * mz);       // metres this frame
            float a = 0.25f + move / 0.01f;                        // 1 cm a frame: no smoothing
            if (a > 1) a = 1;
            for (int k = 0; k < 3; k++) sm[hand].p[k] += (ptr.p[k] - sm[hand].p[k]) * a;
            float dot = 0;
            for (int k = 0; k < 4; k++) dot += sm[hand].q[k] * ptr.q[k];
            float sgn = dot < 0 ? -1.0f : 1.0f, n = 0;
            for (int k = 0; k < 4; k++) { sm[hand].q[k] += (sgn * ptr.q[k] - sm[hand].q[k]) * a; n += sm[hand].q[k] * sm[hand].q[k]; }
            n = sqrtf(n);
            for (int k = 0; k < 4; k++) sm[hand].q[k] /= n;
        } else {
            sm[hand] = ptr;
            have_sm[hand] = 1;
        }
        pthread_mutex_lock(&g_hj_lock);
        g_hj_ptr[hand] = sm[hand];
        pthread_mutex_unlock(&g_hj_lock);
    } else g_hj_failed[hand]++;
    kl_hands_ctx *c = &g_hj_ctx[hand];
    if (c->just_latched)
        fprintf(stderr, "  [hands] %s hand: ARKit joint axis %s %s the back of the fingers "
                        "(agreement %.3f; along the bone: %s %.3f; thumb |dot| %.3f)\n",
                hand ? "right" : "left", kl_hands_axis_name(c->dorsal_axis),
                c->just_latched > 0 ? "is" : "NOT clearly", c->dorsal_score,
                kl_hands_axis_name(c->distal_axis), c->distal_score, c->thumb_dot);
    if (ok) {
        static int first[2];
        if (!first[hand]++) klovrp_hands_log_first(hand, j, &o);
        static double said[2];
        double now = klovrp_mono_now();
        if (now - said[hand] > 5.0) { said[hand] = now; klovrp_hands_log_pose(hand, &o, tracked); }
    }
    // The hand node is the wrist now (kl_ovrp_set_hand_motion stands back) —
    // unless the other hand holds a controller, which makes both Touch.
    if (ok && !klovrp_hands_beside() && __atomic_load_n(&g_hj_state[!hand], __ATOMIC_ACQUIRE) != 2) {
        klovrp_pose v = { o.root.p[0], o.root.p[1], o.root.p[2],
                          o.root.q[0], o.root.q[1], o.root.q[2], o.root.q[3],
                          0, 0, 0, 0, 0, 0, 0 };
        klovrp_derive_motion(&v, &g_hand_hist[hand], 0);
        klovrp_pose_write(&g_hand_pose[hand], &v);
        __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_hj_owner[hand], 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_lock(&g_hj_lock);
    if (ok) {
        g_hj_ovr[hand] = o;
        memcpy(g_hj_raw[hand], j, sizeof j);
        g_hj_time[hand] = kl_ovrp_predicted_display_time();
    } else {
        g_hj_ovr[hand].tracked = 0;         // the last pose stays where it was
    }
    pthread_mutex_unlock(&g_hj_lock);
    __atomic_store_n(&g_hj_state[hand], 1, __ATOMIC_RELEASE);
    if (was == 2)
        fprintf(stderr, "  [hands] %s hand: no controller any more — hands are the input\n",
                hand ? "right" : "left");
}

// Is the hand node this hand's wrist (hands mode, joints published)?
static int klovrp_hand_is_wrist(int hand) {
    return klovrp_hands_mode() && __atomic_load_n(&g_hj_owner[hand], __ATOMIC_ACQUIRE);
}

void kl_ovrp_set_hand_motion(int hand, float px, float py, float pz,
                             float qx, float qy, float qz, float qw,
                             float vx, float vy, float vz,
                             float avx, float avy, float avz) {
    if ((unsigned)hand > 1) return;
    // While the wrist from the hand's joints is the node, the grip pose waits.
    if (klovrp_hand_is_wrist(hand)) return;
    klovrp_stick_move_offset(&px, &pz);
    py -= klovrp_origin_y();
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, vx, vy, vz, avx, avy, avz, 1 };
// A measured velocity is authoritative — nothing is derived here. The
    // history still advances, so a publisher that later drops to the pose-only
    // call differentiates against the right previous sample instead of a gap.
    klovrp_hist_note(&g_hand_hist[hand], &v, 0);
    klovrp_grab_trace(hand, &v);
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
    float e[3]; klovrp_pose_euler_deg(&v, e);
    float pp[3] = { px, py, pz };
    kl_ovrp_ctrl_trace("published", hand, e, pp);
}

    // ...and the pose-only form, which is what the macOS viewer publishes. It
// differentiates rather than passing six zeros to the call above — zeros are the
// claim that a moving controller is stationary (see klovrp_pose.motion_valid).
void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    if ((unsigned)hand > 1) return;
    if (klovrp_hand_is_wrist(hand)) return;
    klovrp_stick_move_offset(&px, &pz);
    py -= klovrp_origin_y();
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0, 0 };
    klovrp_derive_motion(&v, &g_hand_hist[hand], 0);
    klovrp_grab_trace(hand, &v);
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

// Has a frontend ever published a head pose / this hand's pose? The two sticky
// flags the pose seam sets, and nothing else: the OpenXR boot gate polls them
// in a loop (rebelancap's fork, 65fd34e).
int kl_ovrp_head_pose_published(void) {
    return __atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE) ? 1 : 0;
}
int kl_ovrp_hand_pose_published(int hand) {
    if ((unsigned)hand > 1) return 0;
    return __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE) ? 1 : 0;
}

void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y) {
    if ((unsigned)hand > 1) return;
    if (hand == 0) klovrp_stick_move_step(stick_x, stick_y);
    // Bounded change-log: every button-word TRANSITION, so "I pressed A and
    // nothing happened" splits into "the bit never arrived" (frontend mapping)
    // vs "it arrived and the guest ignored it" (guest-side read). Always on -
    // presses are rare, so this is a handful of lines per session.
    // KL_INPUT_LOG_MAX raises the cap (LEGO Bricktales: a session of dialogue
    // clicks spent all 400 before the first build), and each line carries its
    // time, so a press can be put next to what the game logged.
    { static uint32_t last[2] = { 0xffffffffu, 0xffffffffu };
      static unsigned n, cap;
      if (!cap) { const char *c = getenv("KL_INPUT_LOG_MAX");
                  cap = c && atoi(c) > 0 ? (unsigned)atoi(c) : 400; }
      if (buttons != last[hand] && n < cap) { n++;
          fprintf(stderr, "  [input] %.3f hand %d buttons %#x -> %#x (trig %.2f grip %.2f)\n",
                  (double)klovrp_mono_ns() / 1e9,
                  hand, last[hand] == 0xffffffffu ? 0 : last[hand], buttons,
                  (double)index_trigger, (double)hand_trigger);
      }
      last[hand] = buttons; }
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
#define KLOVRP_POKE_MAX 160
static struct klovrp_poke {
    double   t;                       // seconds after the first read
    long     frame;                   // ...or the guest frame, when >= 0
    uint32_t b[2];                    // raw button bits, per hand
    float    idx[2], grip[2];         // triggers, per hand
    float    sx, sy;                  // right thumbstick
    float    lx, ly;                  // left thumbstick (LUP/LDOWN/LLEFT/LRIGHT)
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
        // The left stick: HITMAN 3 VR: Reloaded's mission menu moves its
        // selection with it and not with the right one (Mac, 2026-10-20).
        { "LUP",    0, 0, 0, 0,  0,  1 },
        { "LDOWN",  0, 0, 0, 0,  0, -1 },
        { "LLEFT",  0, 0, 0, 0, -1,  0 },
        { "LRIGHT", 0, 0, 0, 0,  1,  0 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (strlen(t[i].n) != len || strncasecmp(t[i].n, n, len) != 0) continue;
        p->b[t[i].hand] |= t[i].bit;
        if (t[i].idx)  p->idx[t[i].hand]  = 1.0f;
        if (t[i].grip) p->grip[t[i].hand] = 1.0f;
        if (t[i].hand == 1 && t[i].sx) p->sx = t[i].sx;
        if (t[i].hand == 1 && t[i].sy) p->sy = t[i].sy;
        if (t[i].hand == 0 && t[i].sx) p->lx = t[i].sx;
        if (t[i].hand == 0 && t[i].sy) p->ly = t[i].sy;
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
                                "DOWN LEFT RIGHT LUP LDOWN LLEFT LRIGHT\n", (int)(s - w), w);
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
        } else {
            if (p->lx != 0) v.stick_x = p->lx;
            if (p->ly != 0) v.stick_y = p->ly;
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
// The press knobs (KL_OVRP_POKE, KL_OVRP_FAKE_BUTTONS / _TRIGGER) make both hands
// present and tracked at their parked pose: on the Mac no frontend publishes
// one, an OpenXR guest's actions read nothing from an absent hand, and HITMAN 3
// VR: Reloaded draws an untracked controller at the LOCAL origin — in the eye
// once LOCAL is eye-level (2026-10-20).
static int klovrp_fake_present(void) {
    static int fake = -1;
    if (fake < 0) fake = kl_env_uint("KL_OVRP_FAKE_BUTTONS", 0) != 0 || kl_env_str("KL_OVRP_FAKE_TRIGGER", NULL) != NULL
                         || kl_env_str("KL_OVRP_POKE", NULL) != NULL;
    return fake;
}
// KL_OVRP_HAND_SCRIPT="<s>:<L|R>:<dx>,<dy>,<dz>;..." — a host run's hands, placed
// relative to the head (metres, head frame: +x right, +y up, -z ahead) from that
// many seconds on, until that hand's next entry; "<s>:<L|R>:off" hands it back.
// Timed on KL_OVRP_POKE's clock, so the two scripts line up. For a tutorial that
// asks for a gesture — HITMAN 3 VR: Reloaded's "move the item into the wrist
// inventory ring" (Mac, 2026-10-21). Diagnostic; a headset never sets it.
#define KLOVRP_HS_MAX 64
static struct { double t; int hand, off; float d[3]; } g_hs[KLOVRP_HS_MAX];
static int g_hs_n = -1;
static void klovrp_hand_script(int hand, klovrp_pose *p) {
    if (g_hs_n < 0) {
        g_hs_n = 0;
        const char *s = kl_env_str("KL_OVRP_HAND_SCRIPT", NULL);
        while (s && *s && g_hs_n < KLOVRP_HS_MAX) {
            double t; char h; float x, y, z; int n = 0;
            if (sscanf(s, "%lf:%c:off%n", &t, &h, &n) == 2 && n) {
                g_hs[g_hs_n].t = t; g_hs[g_hs_n].hand = (h == 'L' || h == 'l') ? 0 : 1; g_hs[g_hs_n].off = 1; g_hs_n++;
            } else if (sscanf(s, "%lf:%c:%f,%f,%f%n", &t, &h, &x, &y, &z, &n) == 5 && n) {
                g_hs[g_hs_n].t = t; g_hs[g_hs_n].hand = (h == 'L' || h == 'l') ? 0 : 1; g_hs[g_hs_n].off = 0;
                g_hs[g_hs_n].d[0] = x; g_hs[g_hs_n].d[1] = y; g_hs[g_hs_n].d[2] = z; g_hs_n++;
            } else break;
            s = strchr(s, ';');
            if (s) s++;
        }
        if (g_hs_n) fprintf(stderr, "  [ovrp] KL_OVRP_HAND_SCRIPT: %d hand placement(s)\n", g_hs_n);
    }
    if (!g_hs_n) return;
    double now = klovrp_mono_now();
    if (g_poke_t0 == 0.0) g_poke_t0 = now;
    double el = now - g_poke_t0;
    int at = -1;
    for (int i = 0; i < g_hs_n; i++)
        if (g_hs[i].hand == hand && g_hs[i].t <= el && (at < 0 || g_hs[i].t >= g_hs[at].t)) at = i;
    if (at < 0 || g_hs[at].off) return;
    klovrp_pose head = klovrp_head();
    float ox, oy, oz;
    klovrp_qrot(&head, g_hs[at].d[0], g_hs[at].d[1], g_hs[at].d[2], &ox, &oy, &oz);
    p->px = head.px + ox; p->py = head.py + oy; p->pz = head.pz + oz;
    p->qx = head.qx; p->qy = head.qy; p->qz = head.qz; p->qw = head.qw;
    p->vx = p->vy = p->vz = 0; p->avx = p->avy = p->avz = 0;
}

int kl_ovrp_hand_motion(int hand, float *pos, float *quat, float *vel, float *ang) {
    if ((unsigned)hand > 1) return 0;
    klovrp_pose p = klovrp_hand(hand);
    klovrp_hand_script(hand, &p);
    if (pos)  { pos[0] = p.px; pos[1] = p.py; pos[2] = p.pz; }
    if (quat) { quat[0] = p.qx; quat[1] = p.qy; quat[2] = p.qz; quat[3] = p.qw; }
    if (vel)  { vel[0] = p.vx; vel[1] = p.vy; vel[2] = p.vz; }
    if (ang)  { ang[0] = p.avx; ang[1] = p.avy; ang[2] = p.avz; }
    return (klovrp_fake_present() || __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE)) ? 1 : 0;
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
    return (klovrp_fake_present() || __atomic_load_n(&g_hand_set[hand], __ATOMIC_ACQUIRE)) ? 1 : 0;
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

// KL_OVRP_POSE_TIME: fill ovrpPoseStatef.Time (the double at +0x50) with the
// moment the step's sample was latched. It was left 0, which a guest reads as
// "tracking has never produced a sample": AC Nexus drives its physical hand
// bodies (GrabPhysTracker, a joint from the rig to each PlayerHandCollider)
// only when the controller's pose time has moved on, so with a constant 0 the
// visual hands followed the controllers while the bodies that touch things
// stayed at the rig's origin (_lastControllerTime 0 in a managed dump) — the
// start cube could not be touched. Default on for the one target measured.
static int klovrp_pose_time(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_POSE_TIME", t && !strcmp(t, "x5812519008825194"));
    }
    return v;
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
    // KL_OVRP_POSE_TRACE: which THREAD reads which node, in order with the
    // frame loop's own lines — the question being which Update's sample the
    // picture is actually drawn from when the guest samples on two threads.
    if ((node == 0 || node == 1 || node == 2 || node == 9) && klovrp_pose_trace(g_trace_frame)) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        fprintf(stderr, "  [ovrp] posetrace read step=%d node=%d tid=%llu\n",
                step, node, (unsigned long long)tid);
    }
    ovrp_hit("ovrp_GetNodePoseState");
    ovrp_log_arg("ovrp_GetNodePoseState", node, __builtin_return_address(0));
    memset(out, 0, 0x58);
// The pose for the STEP the guest asked about, which is the sample it took
    // at ovrp_Update2 — not a live global re-read twenty times a frame. The
    // argument was being discarded; see klovrp_Update2.
    klovrp_pose head = klovrp_step_head(step);
    if (klovrp_one_pose_active()) klovrp_ifs_observe(&head);
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
        // Through klovrp_eye_rot: the cant frozen with the frame's head pose
        // by one pose per guest frame, the live push otherwise.
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
        // KL_OVRP_HANDS_TAP=<seconds>: from that many seconds after the first
        // hand read, move the RIGHT hand down and up in front of the head, once
        // every 1.2 s — a hand tapping a surface at waist height, for a host
        // run that has nobody to do it (Maestro's "tap the podium twice").
        // KL_OVRP_HANDS_TAP_FOR=<seconds> ends it (default 12). Diagnostic.
        {
            static double tap_from = -2.0, tap_for, t0;
            if (tap_from < -1.5) {
                tap_from = kl_env_float("KL_OVRP_HANDS_TAP", -1.0f);
                tap_for  = kl_env_float("KL_OVRP_HANDS_TAP_FOR", 12.0f);
                t0 = kl_ovrp_predicted_display_time();
            }
            double el = kl_ovrp_predicted_display_time() - t0 - tap_from;
            if (tap_from >= 0.0 && h == 1 && el >= 0.0 && el < tap_for) {
                double ph = fmod(el, 1.2) / 1.2;
                float dy = -0.20f - 0.75f * (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979 * ph));
                float ox, oy, oz;
                klovrp_qrot(&head, 0.12f, dy, -0.45f, &ox, &oy, &oz);
                hand = head;
                hand.px = head.px + ox; hand.py = head.py + oy; hand.pz = head.pz + oz;
                static int said;
                if (!said++) fprintf(stderr, "  [ovrp] hand tap script: right hand now moving "
                                             "between 0.20 and 0.95 m below the head\n");
            }
        }
        p = &hand;
    } else if (klovrp_is_tracker(node) && klovrp_no_trackers()) {
        // A Quest has no tracker here: identity at the origin, not moving.
        static const klovrp_pose none = { 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
        p = &none;
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
    if (klovrp_pose_time()) {
        int ix = klovrp_step_ix(step);
        double t = __atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE)
                       ? g_step_time[ix] : kl_ovrp_predicted_display_time();
        memcpy((char *)out + 0x50, &t, sizeof t);   // ovrpPoseStatef.Time
    }
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
    if ((c == 0 || c == 1 || c == 2 || c == 9) && klovrp_pose_trace(g_trace_frame))
        fprintf(stderr, "  [ovrp] posetrace GetNodePoseState3 step=%d frameIdx=%d node=%d\n", a, b, c);
    // A read that names a frame index is answered from the sample taken FOR
    // that index, when there is one — not from whatever another thread has
    // sampled since (see g_fs).
    klovrp_fsample fs;
    if (a == KLOVRP_STEP_RENDER && __atomic_load_n(&g_fs_tracks, __ATOMIC_ACQUIRE) &&
        klovrp_fs_get(b, &fs)) {
        g_fs_read = &fs;
        uint64_t r = klovrp_GetNodePoseState_impl(a, c, out);
        g_fs_read = NULL;
        return r;
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
    // One pose per guest frame: THE GUEST'S FRAME CYCLE, and the
    // only boundary both of its threads agree on. Called once per frame, and
    // armed before the pacer, because a guest that blocks here must still have
    // opened its cycle. See klovrp_one_pose_arm.
    klovrp_one_pose_arm();
    { static unsigned n; if (n < 48 || n % 300 == 0 || klovrp_pose_trace(n)) {
        uint64_t tid=0; pthread_threadid_np(NULL,&tid);
        fprintf(stderr, "  [ovrp] frameloop WaitToBeginFrame #%u frameIdx=%ld tid=%llu\n",
                n, frame_index, (unsigned long long)tid);
      } n++; }
    // The game thread, once a frame: per-title adjustments to the running
    // engine (kl_ue4.c, KL_MOH_HOVER).
    { extern void kl_ue4_frame_tick(void); kl_ue4_frame_tick(); }
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

// --- The pose-boundary self-test (one pose per guest frame) ------------------
//
// From rebelancap's fork (his kl_ovrp_pose_boundary_selftest; his commit
// a0e966f puts it in `make check`), adapted to this file. The boundary
// (KL_OVRP_POSE_BOUNDARY) is read once into a static, and the feature's state
// is per process, so the caller names the case it has set the environment up
// for and each case is a process of its own — see tests/t_posebound.c and
// kl_ovrp.h.
//
// The measurement is his: the GAMEPLAY pattern, the one index keying failed on
// and the menu pattern passed. Unity's render thread trails its sim thread, so
// the two ovrp_Update2 calls of one frame cycle carry DIFFERENT frame indices,
// and the frontend publishes a new head pose in between (it runs at panel rate
// on its own thread and does not stop for the guest). Every case below takes
// that one measurement and differs only in what it must read off it — which is
// what makes the index case worth running: the same lines that say "one pose"
// under the cycle boundary say "split" there, so the gate is seen to fail.
//
// The last two cases are the other side of the feature: a guest that never
// calls ovrp_WaitToBeginFrame (every OpenXR frame loop) must be given what the
// latch alone gives it, untouched by one-pose — klovrp_one_pose_active.
//
// **Where this port differs from his, and what is therefore NOT asserted.** A
// read that NAMES a frame index (ovrp_GetNodePoseState3) is answered from the
// sample taken for that index, ahead of the frozen pose (klovrp_step_head). So
// "every read in a cycle returns the cycle's pose" holds for reads that name no
// index or name the cycle's own, and a read naming the PREVIOUS frame gets the
// pose that was frozen and filed for the previous frame. That is asserted as
// it is, in the cycle case.
//
// Node 9 is Head, node 0 the left eye. Quaternion at f[0..3], position f[4..6].
typedef struct {
    int cycles;
    int split;         // cycles whose head reads returned more than one pose
    int first_open;    // ...whose first read was the pose published at the cycle's open
    int render_mid;    // ...whose Render-step read was the pose published mid-frame
    int late_first;    // ...whose Physics-step read after the Render sample was the first read's
    int filed_first;   // ...whose filed record was the first read's pose
    int filed_render;  // ...whose filed record was the Render-step read's pose
    int eye_open;      // ...whose eye stood where the offset pushed BEFORE the open put it
    int eye_live;      // ...whose eye stood where the offset pushed mid-frame put it
} klovrp_pb_run;

#define KLOVRP_PB_EYE_OPEN 0.030f     // half the eye separation as the cycle opens
#define KLOVRP_PB_EYE_MID  0.034f     // ...and as pushed again mid-frame

static void klovrp_pb_publish(float yaw, float eye) {
    kl_ovrp_set_head_pose(0, 1.6f, 0, 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f));
    kl_ovrp_set_eye_offset(0, -eye, 0, 0);
    kl_ovrp_set_eye_offset(1,  eye, 0, 0);
}
static int klovrp_pb_same(const float *a, const float *b) {
    for (int i = 0; i < 4; i++) if (fabsf(a[i] - b[i]) > 1e-6f) return 0;
    return 1;
}
static int klovrp_pb_is_yaw(const float *q, float yaw) {
    const float w[4] = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
    return klovrp_pb_same(q, w);
}
// The filed anchor, straight out of the pending record — what kl_reproject
// subtracts.
static void klovrp_pb_filed(float q[4]) {
    pthread_mutex_lock(&g_frames.mu);
    q[0] = g_frames.pending.qx; q[1] = g_frames.pending.qy;
    q[2] = g_frames.pending.qz; q[3] = g_frames.pending.qw;
    pthread_mutex_unlock(&g_frames.mu);
}
static int klovrp_pb_dist_is(const float *a, const float *b, float want) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - want) < 1e-4f;
}

// Four guest frame cycles of the gameplay pattern, and what each one showed.
static void klovrp_pb_trailing(FILE *f, klovrp_pb_run *r) {
    memset(r, 0, sizeof *r);
    for (int cyc = 1; cyc <= 4; cyc++) {
        const int sim = cyc + 1, ren = cyc;      // the render thread trails one
        float a0 = 0.20f * (float)cyc;           // where the head is at cycle open
        float a1 = a0 + 0.05f;                   // ...and where it has got to mid-frame

        klovrp_pb_publish(a0, KLOVRP_PB_EYE_OPEN);
        klovrp_WaitToBeginFrame(cyc);                    // the cycle opens

        klovrp_Update2(0, sim, 0.0);                     // Physics, sim index
        float p_sim[24] = {0};
        klovrp_GetNodePoseState_impl(0, 9, p_sim);

        // The frontend publishes again, mid-frame — head and eye transform.
        klovrp_pb_publish(a1, KLOVRP_PB_EYE_MID);

        klovrp_Update2(KLOVRP_STEP_RENDER, ren, 0.0);    // Render, trailing index
        float p_ren[24] = {0}, p_eye[24] = {0}, p_late[24] = {0};
        klovrp_GetNodePoseState_impl(KLOVRP_STEP_RENDER, 9, p_ren);
        klovrp_GetNodePoseState_impl(KLOVRP_STEP_RENDER, 0, p_eye);
        // ...and one more query after the second step, which is where most of
        // a frame's reads actually land.
        klovrp_GetNodePoseState_impl(0, 9, p_late);

        klovrp_begin_frame_impl(ren);                    // files the anchor
        float fq[4];
        klovrp_pb_filed(fq);

        int split = !klovrp_pb_same(p_sim, p_ren) || !klovrp_pb_same(p_sim, p_late);
        int eye_open = klovrp_pb_dist_is(p_eye + 4, p_ren + 4, KLOVRP_PB_EYE_OPEN);
        int eye_live = klovrp_pb_dist_is(p_eye + 4, p_ren + 4, KLOVRP_PB_EYE_MID);
        r->cycles++;
        r->split        += split;
        r->first_open   += klovrp_pb_is_yaw(p_sim, a0);
        r->render_mid   += klovrp_pb_is_yaw(p_ren, a1);
        r->late_first   += klovrp_pb_same(p_late, p_sim);
        r->filed_first  += klovrp_pb_same(fq, p_sim);
        r->filed_render += klovrp_pb_same(fq, p_ren);
        r->eye_open     += eye_open;
        r->eye_live     += eye_live;
        fprintf(f, "  cycle %d (sim index %d, render index %d): queries %s, filed "
                   "anchor %s, eye transform %s\n",
                cyc, sim, ren,
                split ? "SPLIT ACROSS TWO POSES" : "one pose",
                klovrp_pb_same(fq, p_sim) ? "is the first read's pose"
                    : klovrp_pb_same(fq, p_ren) ? "is the Render step's pose"
                    : "is NEITHER read's pose",
                eye_open ? "as pushed before the cycle opened"
                    : eye_live ? "as pushed mid-frame" : "UNEXPECTED");
    }
}

// Unity's other pattern, the one Beat Saber 1.40 and Arkham Shadow show: both
// steps carry the SAME index, the Render step is sampled a second time after
// BeginFrame (the before-render update, which re-files the open record), and
// the reads name the frame. Only run with the cycle boundary. Returns the
// number of frames that broke it.
static int klovrp_pb_same_index(FILE *f) {
    int bad = 0;
    float prev[4] = {0};
    for (int c = 10; c <= 15; c++) {
        float a = 0.10f * (float)c;
        klovrp_pb_publish(a, KLOVRP_PB_EYE_OPEN);
        klovrp_WaitToBeginFrame(c);
        klovrp_Update2(0, c, 0.0);
        klovrp_pb_publish(a + 0.01f, KLOVRP_PB_EYE_MID);
        klovrp_Update2(KLOVRP_STEP_RENDER, c, 0.0);
        float r1[24] = {0}, r2[24] = {0}, r3[24] = {0}, old[24] = {0}, f1[4], f2[4];
        klovrp_GetNodePoseState3(KLOVRP_STEP_RENDER, c, 9, r1);
        klovrp_pb_publish(a + 0.02f, KLOVRP_PB_EYE_MID);
        klovrp_begin_frame_impl(c);
        klovrp_pb_filed(f1);
        klovrp_pb_publish(a + 0.03f, KLOVRP_PB_EYE_MID);
        klovrp_Update2(KLOVRP_STEP_RENDER, c, 0.0);      // before-render, after Begin
        klovrp_pb_filed(f2);
        klovrp_GetNodePoseState3(KLOVRP_STEP_RENDER, c, 9, r2);
        klovrp_GetNodePoseState_impl(0, 9, r3);          // Physics step, no index
        // A read that names the PREVIOUS frame, inside this cycle.
        klovrp_GetNodePoseState3(KLOVRP_STEP_RENDER, c - 1, 9, old);

        int one = klovrp_pb_is_yaw(r1, a) && klovrp_pb_same(r1, r2) &&
                  klovrp_pb_same(r1, r3) && klovrp_pb_same(r1, f1) &&
                  klovrp_pb_same(r1, f2);
        // Only where the previous frame's sample exists and the by-index read
        // is in force (KL_OVRP_RECORD_INDEXED, and a BeginFrame has found its
        // own index — both true here from the second frame on).
        int check_old = c > 10 && __atomic_load_n(&g_fs_tracks, __ATOMIC_ACQUIRE);
        int old_ok = !check_old ||
                     (klovrp_pb_same(old, prev) && !klovrp_pb_same(old, r1));
        fprintf(f, "  frame %d (both steps index %d, re-sampled after BeginFrame): "
                   "reads and record %s%s\n", c, c,
                one ? "one pose" : "DIFFER",
                !check_old ? ""
                    : old_ok ? "; a read naming the previous frame is answered "
                               "with the pose filed for that frame"
                             : "; a read naming the previous frame is NOT answered "
                               "with the pose filed for it");
        if (!one || !old_ok) bad++;
        memcpy(prev, f2, sizeof prev);
    }
    return bad;
}

// A guest that takes its frames through OpenXR (kl_openxr): kl_ovrp_frame_latch
// at xrWaitFrame, then the head, the eye view and the record opened beside the
// latch — with the frontend publishing in between. What it must be given is
// what the latch alone gives, because such a guest never calls
// ovrp_WaitToBeginFrame and one-pose is not in force for it
// (klovrp_one_pose_active): the latch's pose, and the eye transform live.
//
// `update2` adds Walkabout's shape — Unity on OpenXR with Meta's SDK calling
// ovrp_Update2(step 0, frameIdx 0) in mid-frame, after the frontend has
// published again. The head read before that call and the one after it must be
// one pose. Returns the frames that broke it.
static int klovrp_pb_external(FILE *f, int update2) {
    int bad = 0;
    for (int c = 1; c <= 4; c++) {
        float a = 0.30f * (float)c;
        klovrp_pb_publish(a, KLOVRP_PB_EYE_OPEN);
        kl_ovrp_frame_latch();
        klovrp_pb_publish(a + 0.02f, KLOVRP_PB_EYE_MID);
        float hp[3], hq[4], lp[3], lq[4], ep[3], eq[4], tan[4], fq[4];
        kl_ovrp_get_guest_head_pose(&hp[0], &hp[1], &hp[2], &hq[0], &hq[1], &hq[2], &hq[3]);
        klovrp_pb_publish(a + 0.05f, KLOVRP_PB_EYE_MID);
        if (update2) klovrp_Update2(0, 0, 0.0);
        klovrp_pb_publish(a + 0.07f, KLOVRP_PB_EYE_MID);
        kl_ovrp_get_guest_head_pose(&lp[0], &lp[1], &lp[2], &lq[0], &lq[1], &lq[2], &lq[3]);
        kl_ovrp_eye_view(0, &ep[0], &ep[1], &ep[2], &eq[0], &eq[1], &eq[2], &eq[3], tan);
        kl_ovrp_frame_begin_external();
        klovrp_pb_filed(fq);
        int one  = klovrp_pb_same(hq, lq);
        int head = klovrp_pb_is_yaw(hq, a) && one && klovrp_pb_same(hq, fq) &&
                   (klovrp_eye_cant() || klovrp_pb_same(hq, eq));
        int eye  = klovrp_pb_dist_is(ep, lp, KLOVRP_PB_EYE_MID);
        fprintf(f, "  frame %d (%s): head %s, %s; eye transform %s\n", c,
                update2 ? "latch, then ovrp_Update2(step 0, frameIdx 0) mid-frame"
                        : "no OVRPlugin calls",
                !one ? (update2 ? "CHANGES at the Update2" : "CHANGES inside the frame")
                     : update2 ? "before and after the Update2 is one pose"
                               : "is one pose",
                head ? "the one published at the latch, as are eye view and record"
                     : "and head, eye view and record are NOT all the latch's pose",
                eye ? "as pushed since (live)" : "NOT the live one");
        if (!head || !eye) bad++;
    }
    return bad;
}

// None of the feature's state valid, none of its counters moved.
static int klovrp_pb_untouched(FILE *f) {
    uint64_t served = 0;
    for (int i = 0; i < KLOP_N; i++) served += g_op_served[i];
    int state = __atomic_load_n(&g_op_valid, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_eye_frame_valid, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_op_armed, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_op_arm_driven, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_op_index_keyed, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_ifs_have, __ATOMIC_ACQUIRE) || g_ser_have;
    uint64_t counted = g_op_freezes + g_op_reuses + g_op_arms + served +
                       g_op_fellthrough + g_op_div_n + g_eye_rot_pushes;
    fprintf(f, "  one-pose state %s, counters %s\n",
            state ? "BECAME VALID" : "untouched (nothing frozen, nothing armed)",
            counted ? "MOVED" : "all zero");
    return !state && !counted;
}

int kl_ovrp_pose_boundary_selftest(FILE *f, int which) {
    int ok = 1;
    if (!f) f = stderr;

    // The case must be the one the process was started for: the boundary is
    // read once, so a wrong environment would otherwise pass or fail for a
    // reason that has nothing to do with the code. It is only read (and only
    // matters) for a guest that arms.
    if (which == KL_OVRP_POSEBOUND_CYCLE || which == KL_OVRP_POSEBOUND_INDEX) {
        int cycle = klovrp_pose_boundary_cycle();
        if (cycle != (which != KL_OVRP_POSEBOUND_INDEX)) {
            fprintf(f, "  FAIL: this case needs KL_OVRP_POSE_BOUNDARY=%s, and it "
                       "reads %s\n", which != KL_OVRP_POSEBOUND_INDEX ? "cycle" : "index",
                    cycle ? "cycle" : "index");
            return 0;
        }
    }

    klovrp_pb_run r;
    switch (which) {
    case KL_OVRP_POSEBOUND_CYCLE: {
        // (a) One pose per ovrp_WaitToBeginFrame cycle, it is the pose that
        // stood at the cycle's open, the record carries it, and the eye
        // transform is the one that stood there too.
        klovrp_pb_trailing(f, &r);
        if (r.split || r.first_open != r.cycles || r.filed_first != r.cycles ||
            r.eye_open != r.cycles) ok = 0;
        // One freeze per cycle, no more and no fewer. The counters are only
        // reset by the 300-freeze census, which cannot have fired here.
        fprintf(f, "  freezes %llu over %llu guest frame cycles (must be equal)\n",
                (unsigned long long)g_op_freezes, (unsigned long long)g_op_arms);
        if (g_op_arms != (uint64_t)r.cycles || g_op_freezes != g_op_arms) ok = 0;

        if (klovrp_pb_same_index(f)) ok = 0;
        fprintf(f, "  freezes %llu over %llu guest frame cycles (must be equal)\n",
                (unsigned long long)g_op_freezes, (unsigned long long)g_op_arms);
        if (g_op_freezes != g_op_arms) ok = 0;
        if (!__atomic_load_n(&g_op_valid, __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&g_eye_frame_valid, __ATOMIC_ACQUIRE)) ok = 0;
        break;
    }
    case KL_OVRP_POSEBOUND_INDEX: {
        // (b) His finding, reproduced: keyed on the frame index the second
        // Update2 of every cycle re-freezes, the cycle's reads are answered
        // from two heads and the record carries the second. Passing HERE means
        // the split was seen — the measurement of case (a) can fail.
        klovrp_pb_trailing(f, &r);
        fprintf(f, "  freezes %llu over %llu guest frame cycles (two per cycle "
                   "is the defect)\n",
                (unsigned long long)g_op_freezes, (unsigned long long)g_op_arms);
        if (r.split != r.cycles || r.first_open != r.cycles ||
            r.render_mid != r.cycles || r.filed_render != r.cycles ||
            r.filed_first != 0) ok = 0;
        if (g_op_arms != (uint64_t)r.cycles || g_op_freezes != 2 * g_op_arms) ok = 0;
        break;
    }
    case KL_OVRP_POSEBOUND_XR:
        // (c) A guest that never speaks OVRPlugin: nothing arms, so one-pose is
        // not in force — the latch's pose and the live eye transform, and
        // nothing frozen.
        if (klovrp_pb_external(f, 0)) ok = 0;
        if (!klovrp_pb_untouched(f)) ok = 0;
        break;
    case KL_OVRP_POSEBOUND_MIXED:
        // (d) Walkabout's shape: the ovrp_Update2 in mid-frame does not change
        // the head the OpenXR side reads, it is the latch's pose throughout,
        // and none of the feature's state moves.
        if (klovrp_pb_external(f, 1)) ok = 0;
        if (!klovrp_pb_untouched(f)) ok = 0;
        break;
    default:
        fprintf(f, "  FAIL: no such case %d\n", which);
        ok = 0;
    }
    return ok;
}

        // ovrpVector3f by value — a 12-byte HFA, so the floats go home in s0..s2,
// which a uint64 return would never set. Returning a three-float struct from
// C makes Clang emit exactly the real plugin's `ldp s0, s1 / ldr s2 / ret`.
// Zeros are also what the real plugin returns when no boundary exists.
typedef struct { float x, y, z; } klovrp_vec3;
static klovrp_vec3 klovrp_GetBoundaryDimensions(void) {
    ovrp_hit("ovrp_GetBoundaryDimensions");
    float d[3];
    if (klovrp_boundary(d)) {
        static int said;
        if (!said++) fprintf(stderr, "  [ovrp] ovrp_GetBoundaryDimensions asked (KL_OVRP_BOUNDARY)\n");
        return (klovrp_vec3){d[0], d[1], d[2]};
    }
    return (klovrp_vec3){0, 0, 0};
}

// Controller masks (metadata OVRPlugin.Controller): LTouch=1, RTouch=2,
// Touch=3, Remote=4, Gamepad=0x10, LHand=0x20, RHand=0x40, Hands=0x60,
// Active=0x80000000. Only the Touch controllers exist here, so a mask asking
// for anything else (Remote/Gamepad/Hands) connects nothing. Active expands
// to the controllers that are present.
#define OVRP_CTRL_LTOUCH 0x1u
#define OVRP_CTRL_RTOUCH 0x2u
#ifndef OVRP_CTRL_TOUCH
#define OVRP_CTRL_TOUCH (OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH)
#endif
#define OVRP_CTRL_ACTIVE 0x80000000u

// The frontend hands touches over in ovrpButton bit values ("this button is
// touched"), which is what the OpenXR path reads. The OVRPlugin struct wants
// ovrpTouch and ovrpNearTouch, which number things differently:
//   ovrpTouch     A 0x1  B 0x2  RThumbstick 0x4  RIndexTrigger 0x10
//                 X 0x100  Y 0x200  LThumbstick 0x400  LIndexTrigger 0x1000
//   ovrpNearTouch LIndexTrigger 0x1  LThumbButtons 0x2
//                 RIndexTrigger 0x4  RThumbButtons 0x8
// The face buttons and stick clicks coincide; the triggers do not, and the
// button word's stick DIRECTION bits land on the two trigger-touch bits. Passed
// through raw, a trigger touch is invisible and pushing a stick reads as
// touching a trigger. Unity's legacy joystick maps trigger TOUCH to buttons
// 14 (L) / 15 (R), which is the select Air Brigade 2's menu reads.
#define KLOVRP_TOUCH_FACE 0x00000707u   // A B RThumbstick X Y LThumbstick
static uint32_t klovrp_touch_bits(uint32_t t) {
    uint32_t r = t & KLOVRP_TOUCH_FACE;
    if (t & 0x04000000u) r |= 0x10u;        // RIndexTrigger
    if (t & 0x10000000u) r |= 0x1000u;      // LIndexTrigger
    return r;
}
static uint32_t klovrp_neartouch_bits(uint32_t t) {
    uint32_t r = 0;
    if (t & 0x10000000u) r |= 0x1u;                     // LIndexTrigger
    if (t & (0x100u | 0x200u | 0x400u)) r |= 0x2u;      // LThumbButtons
    if (t & 0x04000000u) r |= 0x4u;                     // RIndexTrigger
    if (t & (0x1u | 0x2u | 0x4u)) r |= 0x8u;            // RThumbButtons
    return r;
}

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

static int klovrp_hands_mode(void);
static uint32_t klovrp_hands_buttons(void);
static uint32_t klovrp_hands_beside_bits(void);
#define OVRP_CTRL_HANDS 0x60u

static void fill_controller_state(int mask, void *out, int version) {
    memset(out, 0, version >= 4 ? 0x60 : version == 2 ? 0x40 : 0x30);
    uint32_t m = (uint32_t)mask;
    // Hand tracking as the input (see kl_ovrp_set_hand_joints): the hands are
    // what is connected, not a Touch pair. Elixir decides from this field
    // alone whether to ask for hands — it never calls GetHandTrackingEnabled.
    if (klovrp_hands_mode()) {
        uint32_t *hw = (uint32_t *)out;
        hw[0] = (m & OVRP_CTRL_ACTIVE) ? OVRP_CTRL_HANDS : (m & OVRP_CTRL_HANDS);
        if (hw[0]) hw[1] = hw[2] = klovrp_hands_buttons();
        return;
    }
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
    // Start is a LEFT-hand button on a Touch pair, and a guest that polls the
    // hands one mask at a time looks for it only there. The right Sense pill
    // (Options) publishes it too — see KleptonControllers.swift — so carry it
    // across, or it is seen by a both-hands poll and missed by a left-only one.
    if (in[1].buttons & KL_OVRP_RAW_START) {
        in[0].buttons |= KL_OVRP_RAW_START;
        in[0].touches |= KL_OVRP_RAW_START;
    }
    static int all_connected = -1;
    if (all_connected < 0) {
        const char *t = kl_driver_target_name();
        all_connected = kl_env_on("KL_OVRP_CONNECTED_CONTROLLERS_ALL",
                                  t && strcmp(t, "x2371757336252737") == 0);
    }
    w[0] = all_connected ? (uint32_t)OVRP_CTRL_TOUCH : conn; // ConnectedControllers
    // Hands offered beside the controllers (KL_OVRP_HANDS_BESIDE): connected
    // too while seen, for a mask that asks for them.
    w[0] |= klovrp_hands_beside_bits() &
            (((uint32_t)mask & OVRP_CTRL_ACTIVE) ? OVRP_CTRL_HANDS : (uint32_t)mask);
    if (conn & OVRP_CTRL_LTOUCH) {
        w[1] |= in[0].buttons;                          // Buttons
        w[2] |= klovrp_touch_bits(in[0].touches);       // Touches
        w[3] |= klovrp_neartouch_bits(in[0].neartouches); // NearTouches
        f[4] = in[0].index_trigger;                     // LIndexTrigger
        f[6] = in[0].hand_trigger;                      // LHandTrigger
        f[8] = in[0].stick_x; f[9] = in[0].stick_y;
        if (version >= 2) { f[12] = in[0].stick_x; f[13] = in[0].stick_y; }
        if (version >= 4) {
            int level = kl_ovrp_controller_battery_level(0);
            b[0x40] = (uint8_t)(level >= 0 ? level : 100); // LBatteryPercentRemaining
        }
    }
    if (conn & OVRP_CTRL_RTOUCH) {
        w[1] |= in[1].buttons;
        w[2] |= klovrp_touch_bits(in[1].touches);
        w[3] |= klovrp_neartouch_bits(in[1].neartouches);
        f[5] = in[1].index_trigger;                     // RIndexTrigger
        f[7] = in[1].hand_trigger;                      // RHandTrigger
        f[10] = in[1].stick_x; f[11] = in[1].stick_y;
        if (version >= 2) { f[14] = in[1].stick_x; f[15] = in[1].stick_y; }
        if (version >= 4) {
            int level = kl_ovrp_controller_battery_level(1);
            b[0x41] = (uint8_t)(level >= 0 ? level : 100); // RBatteryPercentRemaining
        }
    }
    if (all_connected && version >= 4) {
        b[0x40] = 100;                                  // LBatteryPercentRemaining
        b[0x41] = 100;                                  // RBatteryPercentRemaining
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

// Newest shape: ovrpControllerState5, (mask=w0, out=x1), plain ovrpResult. It is
// ovrpControllerState4 with a longer tail (thumb-rest / stylus force, index
// trigger curl/slide/force) used only by Touch Pro / stylus hardware. We share
// the v4 fill: it zeroes and fills the common 0x60 prefix, and the v5-only tail
// past 0x60 stays 0 — which is exactly right (no force, no curl on a plain Touch
// mapping). The guest marshals the struct as a zero-initialised value type, so
// leaving the tail untouched is safe and needs no assumption about its exact
// size beyond it being >= 0x60. wrath2 / UE4 polls this one.
static uint64_t klovrp_GetControllerState5(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState5");
    ovrp_log_arg("ovrp_GetControllerState5", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 5);
    ctrl_state_log("GetControllerState5", mask, out);
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

// OVRPlugin 1.28 ABI:
//   ovrpResult ovrp_GetDominantHand(ovrpHandedness *dominantHand)
// visionOS does not expose the Meta account's handedness preference. Use the
// conventional right-handed default instead of aborting a healthy title while
// it constructs its main menu. Enum values: unsupported=0, left=1, right=2.
static int32_t klovrp_GetDominantHand(int32_t *out) {
    ovrp_hit("ovrp_GetDominantHand");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 2;
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
_Static_assert(offsetof(ovrp_layer_desc_eyefov, mv_format) == 0x6c, "desc");

// **How much of it is the guest's.** The three motion-vector fields came with
// ovrp_CalculateEyeLayerDesc3, and a game built against an older plugin owns
// only the first 0x6c bytes: its desc is a stack local of that size. Measured in
// the plugins the games ship: 1.30, 1.44, 1.45 and 1.46 copy 0x6c in
// Compositor::ExportEyeLayerDesc and their builder clears 0x64 from +0x08;
// 1.51 (the first with ...Desc3) clears 0x74 from +0x08, i.e. all 0x7c.
// Writing the whole struct into a 1.44 guest ran 16 bytes past its desc — Star
// Wars Pinball VR (x3180994211970184, UE 4.25) keeps it at sp+0x40 in
// FOculusHMD::UpdateStereoRenderingParams with the stack guard at sp+0xb0, and
// died on "stack smashing detected" at its first eye layer. Vader Immortal I
// (OVRPlugin 1.30.0, UE 4.19) had no guard to trip: its 0x70-byte desc sits right
// below the saved d8 that carries the world delta, mv_depth_format landed on it,
// and every UWorld::Tick ran at 0.0005 s — the 16-second intro took 33,000
// frames, five and a half minutes of black. KL_OVRP_EYE_DESC_SHORT
// =1/0 forces either size; unset, a game whose plugin is older than 1.51 gets
// the short one, and a game of unknown version the whole struct, as before.
static size_t klovrp_guest_desc_size(void) {
    static size_t n;
    if (!n) {
        const char *v = klovrp_guest_version();
        int major = 0, minor = 0;
        int old = v && sscanf(v, "%d.%d", &major, &minor) == 2 && major == 1 && minor < 51;
        n = kl_env_on("KL_OVRP_EYE_DESC_SHORT", old)
                ? offsetof(ovrp_layer_desc_eyefov, mv_format) : sizeof(ovrp_layer_desc_eyefov);
        if (n < sizeof(ovrp_layer_desc_eyefov))
            fprintf(stderr, "  [ovrp] layer desc: %#zx bytes, the size OVRPlugin %s has "
                            "(no motion-vector fields before 1.51; KL_OVRP_EYE_DESC_SHORT)\n",
                    n, v ? v : "?");
    }
    return n;
}

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

// See kl_ovrp.h. Side-effect free: no pose slot is consumed.
void kl_ovrp_eye_tangents(int eye, float out[4]) {
    if (!out) return;
    if ((unsigned)eye > 1) eye = 0;
    memcpy(out, klovrp_eye_tan(eye), sizeof g_eye_tan[eye]);
}

// See kl_ovrp.h. Read from the first eye layer whose desc carries a frustum.
int kl_ovrp_eye_layer_fov(int eye, float out[4]) {
    if (!out || (unsigned)eye > 1) return 0;
    for (int i = 0; i < KLOVRP_MAX_LAYERS; i++) {
        const struct klovrp_layer *l = &g_layers[i];
        if (!l->id || !l->is_eye) continue;
        const ovrp_fovf *f = &l->desc.fov[eye];
        if (!(f->left > 0 && f->right > 0 && f->up > 0 && f->down > 0)) continue;
        out[0] = f->left; out[1] = f->right; out[2] = f->up; out[3] = f->down;
        return 1;
    }
    return 0;
}

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
// Eye frames ended since an Equirect layer was last submitted — see
// kl_loadingvideo_active. Written from the guest's main and render threads and
// read by the compositor; a stale read costs one frame of backdrop either way.
#define KLOVRP_LOADVID_GRACE_FRAMES 6
static volatile unsigned g_loadvid_frames_since;
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
// The video a guest asked to play into its Equirect layer (kl_jni_ue4.c,
// GameActivity.playVideo), as a host path, and a generation that moves with
// every change so the compositor can tell a new video from the same one.
static pthread_mutex_t g_loadvid_src_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_loadvid_src[1024];
static unsigned g_loadvid_src_gen;
void kl_loadingvideo_set_source(const char *host_path) {
    pthread_mutex_lock(&g_loadvid_src_lock);
    snprintf(g_loadvid_src, sizeof g_loadvid_src, "%s", host_path ? host_path : "");
    g_loadvid_src_gen++;
    pthread_mutex_unlock(&g_loadvid_src_lock);
    fprintf(stderr, "  [ovrp] loading video: %s\n", host_path && *host_path ? host_path : "(none)");
}
unsigned kl_loadingvideo_source(char *out, int cap) {
    pthread_mutex_lock(&g_loadvid_src_lock);
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", g_loadvid_src);
    unsigned gen = g_loadvid_src_gen;
    pthread_mutex_unlock(&g_loadvid_src_lock);
    return gen;
}
// Swift-facing: 1 while the guest has a live Equirect loading layer.
int kl_loadingvideo_active(void) {
    // KL_LOADVID_OFF=1: force the loading skybox OFF regardless of whether the
    // Equirect loading layer is still live. AC Nexus never destroys that layer
    // once the menu is up, so the skybox stays painted over the (working) menu
    // overlays. This knob lets a run reveal the menu behind it without a rebuild;
    // the proper fix is to deactivate the skybox when the menu is being submitted.
    if (kl_env_on("KL_LOADVID_OFF", 0)) return 0;
    // ...and this is that fix. The layer's EXISTENCE is the wrong signal: the
    // guest keeps its Equirect layers allocated and simply stops SUBMITTING
    // them when its loading screen is hidden. So once eye frames are flowing,
    // a run of them with no Equirect submitted means the guest is showing its
    // own picture and the backdrop has to get out of the way. Counted in the
    // guest's own frames (klovrp_EndFrame4), not in time, so a load that stalls
    // the main thread for a second does not uncover a stale eye — no frame
    // ends, the count does not move. With no eye frames at all (a guest whose
    // display never came up) it never trips and the old behaviour stands.
    // KL_LOADVID_FOLLOW_SUBMIT=0 restores "live while the layer exists".
    static int follow = -1, shown = -1;
    if (follow < 0) follow = kl_env_on("KL_LOADVID_FOLLOW_SUBMIT", 1);
    int on = g_loadvid_active &&
             !(follow && g_loadvid_frames_since >= KLOVRP_LOADVID_GRACE_FRAMES);
    if (on != shown && g_loadvid_active) {
        shown = on;
        fprintf(stderr, "  [ovrp] loading-video skybox %s (the guest %s its Equirect "
                        "layer)\n", on ? "shown" : "hidden",
                on ? "is submitting" : "stopped submitting");
    }
    return on;
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
//
// ...and 0xb0 is only where RE4's union arm starts. The header grew: the
// OVRPlugin wrath2's UE4 was built against carries the blend-factor triple
// (OverrideBlendFactors, Src, Dst — the has_blend_factors/src/dst fields our
// own struct got from libOculusXRPlugin's DWARF) INSIDE the common header, so
// its Quad.Size lands at +0xbc. Read out of wrath2's own
// FLayer::UpdateLayer_RHIThread (0x9c6137c): the Quad arm stores at
// submit+0xbc/+0xc0, the Cylinder arm at +0xbc/+0xc0/+0xc4, where RE4's
// ImportLayerSubmit reads +0xb0. Nothing in the submit SAYS which build wrote
// it, so it is told apart by what the bytes can be: a real quad size is a pair
// of plausible positive floats, and the blend triple is a bool and two small
// enums — bit patterns that read as denormals (~1e-44), never as metres. The
// verdict is latched for the run (the layout is a compile-time property of the
// guest), and until a submit decides it, a zero size is handed on — which is
// exactly what the undecidable submit carries at both offsets anyway.
static int klovrp_quad_size_off;    // 0 = undecided; else 0xb0 or 0xbc

static int klovrp_plausible_metres(float v) {
    return v > 1e-3f && v < 1e5f;   // NaN/inf fail both compares
}
static void klovrp_quad_size(const ovrp_layer_submit *s, float out[2]) {
    const unsigned char *base = (const unsigned char *)s;
    float b0[2], bc[2];
    unsigned int w0[3];
    memcpy(b0, base + 0xb0, sizeof b0);
    memcpy(bc, base + 0xbc, sizeof bc);
    memcpy(w0, base + 0xb0, sizeof w0);
    if (!klovrp_quad_size_off) {
        int old_fits = klovrp_plausible_metres(b0[0]) && klovrp_plausible_metres(b0[1]);
        int new_fits = klovrp_plausible_metres(bc[0]) && klovrp_plausible_metres(bc[1])
                       && w0[0] <= 1 && w0[1] <= 32 && w0[2] <= 32;
        if (old_fits == new_fits) { out[0] = out[1] = 0.0f; return; }   // undecidable
        klovrp_quad_size_off = old_fits ? 0xb0 : 0xbc;
        fprintf(stderr, "  [ovrp] layer submit union starts at +0x%x — the guest's "
                        "OVRPlugin header %s the blend-factor triple (quad size "
                        "%.3fx%.3f m)\n",
                klovrp_quad_size_off, old_fits ? "predates" : "carries",
                old_fits ? b0[0] : bc[0], old_fits ? b0[1] : bc[1]);
    }
    const float *q = klovrp_quad_size_off == 0xb0 ? b0 : bc;
    out[0] = q[0]; out[1] = q[1];
}
// The Cylinder arm of the same union: ArcWidth, Height, Radius, three floats
// where the Quad arm has its two — at +0xb0 in a guest built against the older
// header (read out of RE4's ImportLayerSubmit, above) and at +0xbc in one with
// the blend-factor triple (wrath2's UpdateLayer_RHIThread stores the cylinder
// arm at +0xbc/+0xc0/+0xc4). The offset is the one a quad already latched; a
// guest whose first overlay is a cylinder is decided here by the same test.
// Tales from the Galaxy's Edge puts its main menu on one (2026-10-11).
static void klovrp_cylinder(const ovrp_layer_submit *s, float out[3]) {
    const unsigned char *base = (const unsigned char *)s;
    float b0[3], bc[3];
    unsigned int w0[3];
    memcpy(b0, base + 0xb0, sizeof b0);
    memcpy(bc, base + 0xbc, sizeof bc);
    memcpy(w0, base + 0xb0, sizeof w0);
    out[0] = out[1] = out[2] = 0.0f;
    if (!klovrp_quad_size_off) {
        int old_fits = klovrp_plausible_metres(b0[0]) && klovrp_plausible_metres(b0[1])
                       && klovrp_plausible_metres(b0[2]);
        int new_fits = klovrp_plausible_metres(bc[0]) && klovrp_plausible_metres(bc[1])
                       && klovrp_plausible_metres(bc[2])
                       && w0[0] <= 1 && w0[1] <= 32 && w0[2] <= 32;
        if (old_fits == new_fits) return;                               // undecidable
        klovrp_quad_size_off = old_fits ? 0xb0 : 0xbc;
        fprintf(stderr, "  [ovrp] layer submit union starts at +0x%x (decided by a cylinder)\n",
                klovrp_quad_size_off);
    }
    const float *c = klovrp_quad_size_off == 0xb0 ? b0 : bc;
    if (!klovrp_plausible_metres(c[0]) || !klovrp_plausible_metres(c[1]) ||
        !klovrp_plausible_metres(c[2])) return;
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    static int said;
    if (said < 3) {
        said++;
        fprintf(stderr, "  [ovrp] cylinder layer %d: arc %.3f m, height %.3f m, radius %.3f m "
                        "(%.1f degrees) at (%.2f, %.2f, %.2f)\n", s->layer_id, c[0], c[1], c[2],
                c[0] / c[2] * 57.29578f, s->pose[4], s->pose[5], s->pose[6]);
    }
}

static void klovrp_probe_submit_tail(const ovrp_layer_submit *s) {
    const float *q0 = (const float *)((const unsigned char *)s + 0xb0);
    const float *q1 = (const float *)((const unsigned char *)s + 0xbc);
    fprintf(stderr, "          union +0xb0: %.4f %.4f  +0xbc: %.4f %.4f  "
                    "(ovrpLayerSubmitQuad.Size, metres; layout latched %s)\n",
            q0[0], q0[1], q1[0], q1[1],
            klovrp_quad_size_off == 0xb0 ? "+0xb0"
            : klovrp_quad_size_off == 0xbc ? "+0xbc" : "undecided");
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

// ovrpShape: ReconstructionPassthrough (the whole view) and
// SurfaceProjectedPassthrough (onto a mesh the guest hands over). The second
// is treated as the first: where the eye picture is transparent the room
// shows, whether or not the guest's mesh covers that spot — see "Insight
// Passthrough" further down.
static int klovrp_shape_is_passthrough(int shape) { return shape == 6 || shape == 7; }

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
// ...except in the OVRPlugin Unreal 4.26 links (Saints & Sinners,
// x2897337400373711), where it is bit 0. Measured with KL_OVRP_LAYERS over a
// whole session: every world-placed panel (title, menu, dialogue choices)
// submits flags 0x8, and exactly two kinds submit 0x9 — the full-view fade
// quad 27 cm in front of the head and the health/stamina HUD 33 cm in front
// and 15 cm below it, both at head-relative positions. Read world-locked, both
// sank to the floor once the guest switched to FloorLevel: the fade covered
// only the lower half of the view and the HUD showed far below it.
//
// Skydance's BEHEMOTH (UE5, x5767635983306831; fix 21, sent in by another
// user the same day) found the same bit from the other side: the only quads
// it submits with bit 0 are its transition fade (2.56 m square, 0.27 m in
// front of the eyes) and its loading card, both at the same spot whatever the
// head does, and every quad without it is placed in the room — read
// world-locked, the fade sat at the floor and darkened the lower half of the
// view. Its sender took the bit for NoDepth and made the rule "a NoDepth quad
// is head-locked" for that title.
//
// Two titles, two engine generations, one bit: in the layer list EndFrame is
// handed, bit 0 is HeadLocked. As far as the public headers are remembered
// that is what they say too (ovrpLayerSubmitFlags: HeadLocked 1<<0, ReverseZ
// 1<<3 — which is exactly the 0x8 and 0x9 above), while the flags Unity
// passes to ovrp_EnqueueSubmitLayer have HeadLocked at 0x2, where AC Nexus
// was verified; that path keeps 0x2.
//
// So it is the rule for Unreal guests under a store-built target (x<app id>),
// read beside 0x2. The Unreal targets with a name of their own were brought
// up with 0x2 alone and nobody has looked at their layers with this in mind;
// they keep it until someone does. KL_OVRP_HEADLOCK_BIT0=0|1 overrides.
static int klovrp_head_locked_flags(int flags) {
    static int bit0 = -1;
    if (bit0 < 0) {
        extern kl_guest_kind kl_driver_kind(void);
        const char *t = kl_driver_target_name();
        int store_built = t && t[0] == 'x' && t[1] && strspn(t + 1, "0123456789") == strlen(t + 1);
        bit0 = kl_env_on("KL_OVRP_HEADLOCK_BIT0", store_built && kl_driver_kind() == KL_GUEST_UE4);
    }
    return (flags & (KLOVRP_SUBMIT_HEAD_LOCKED | (bit0 ? 0x1 : 0))) != 0;
}

// KL_OVRP_ENQ_OWNS_OVERLAYS: a guest that files its panels through
// ovrp_EnqueueSubmitLayer hands ovrp_EndFrame4 a list holding the eye layer and
// nothing else, on another thread. Read as "this frame has no overlays", that
// list emptied the published set every KLOVRP_OVERLAY_TTL frames, and the
// panels were gone until the next enqueue put them back — AC Nexus's cards
// blinking every few seconds, seen on device. While enqueue submits are
// arriving the enqueue path owns the set (it publishes an empty one itself
// when every panel is hidden); once they stop for a few frames this takes the
// panels down as before. Default on for the one target measured.
#define KLOVRP_ENQ_LIVE_FRAMES 6
static int klovrp_enq_owns_overlays(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_ENQ_OWNS_OVERLAYS", t && !strcmp(t, "x5812519008825194"));
    }
    return v && __atomic_load_n(&g_enq_frames_since, __ATOMIC_RELAXED) < KLOVRP_ENQ_LIVE_FRAMES;
}

// Panels a guest destroys are taken down at once, from the visible set and from
// the batch being gathered, and the batch is dropped when enqueue submits stop
// (klovrp_enq_age, klovrp_DestroyLayer). Without it Ghost Town's fade quads stay
// published after the game has destroyed them: a white sheet over the picture.
// From community PR #41 (sibest). Default on for Ghost Town only; every other
// guest keeps the earlier lifecycle. KL_OVRP_ENQ_RETIRE=0/1 overrides.
static int klovrp_enq_retire(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_ENQ_RETIRE", t && !strcmp(t, "x9531494193591052"));
    }
    return v;
}

static void klovrp_record_overlays(const void *layer_submits, int count) {
    const ovrp_layer_submit *const *list = layer_submits;
    kl_ovrp_overlay tmp[KLOVRP_MAX_LAYERS];
    int n = 0;
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
            // Twice per LAYER rather than six times in all: one layer that is
            // submitted from the first frame (a splash quad) used to spend the
            // whole quota before the layer in question existed.
            static int on = -1;
            static unsigned char per_layer[64];
            if (on < 0) on = kl_env_on("KL_OVRP_SUBMIT_DUMP", 1);
            struct klovrp_layer *dl = klovrp_layer(s->layer_id);
            unsigned slot = (unsigned)s->layer_id % 64u;
            if (on && per_layer[slot] < 2 && dl && !dl->is_eye) {
                per_layer[slot]++;
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
        // An Equirect layer handed in through the frame's own list counts as
        // submitted for the loading backdrop, the same as one that came through
        // ovrp_EnqueueSubmitLayer (see kl_loadingvideo_active). Without this the
        // backdrop would leave after a few frames on every guest that submits
        // its loading layer this way — the count is only ever reset there.
        if (l && !l->is_eye && l->desc.shape == 5) g_loadvid_frames_since = 0;
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
        // A passthrough layer has no picture of its own to composite — the
        // room is the system's (klovrp_pt_submits).
        if (klovrp_shape_is_passthrough(l->desc.shape)) continue;
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
        o->head_locked = klovrp_head_locked_flags(s->submit_flags);
        // An OVRPlugin layer's storage is the guest's own, and which way up it
        // is follows from the API it rendered with — the same question, and the
        // same answer, as the eye textures beside it. Only the Vulkan path
        // backs a non-eye layer with anything a compositor can sample today
        // (kl_vulkan_layer_mtl_texture), and Vulkan's origin is the top left.
        o->origin_top_left = kl_vulkan_guest_active();
// The quad's world size, out of the union's own arm — which starts at
        // +0xb0 or +0xbc depending on which OVRPlugin the guest was built
        // against. klovrp_quad_size tells them apart and latches; see it.
        if (o->shape == 0)
            klovrp_quad_size(s, o->size);
        else if (o->shape == 1)
            klovrp_cylinder(s, o->cyl);
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
    }
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
    if (n == 0 && klovrp_enq_owns_overlays()) return;
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
// second producer is how the two answers drift.
//
// Except that one process does run both: a Unity OpenXR guest whose
// OVRPlugin (UnityOpenXR mode) still files OVROverlay panels through
// ovrp_EnqueueSubmitLayer2. Meta's runtime appends those to the app's
// xrEndFrame; here the enqueue path published them on its own and every
// xrEndFrame then replaced them with its own quads (none). Mission: ISS's
// training loading screen — controls card, side panels, "pull the trigger" —
// flashed up for a fraction of a frame and was black in between. With
// KL_OVRP_ENQ_WITH_XR (default on for Mission: ISS) the enqueue path keeps its
// set aside and the OpenXR frame publishes its quads plus that set; the
// enqueue set is aged on the OpenXR frame boundary (kl_ovrp_openxr_frame_end),
// so panels the guest stops filing still come down. Fluid (x6946182742093178)
// draws its whole browser UI as such panels over a black eye picture: without
// it, black from start to finish (2026-10-21). Spatial Zombies (x24151641901194680)
// files its menu as such a Cylinder: the pointer reached it and clicked, and
// nothing was drawn (2026-10-28).
static void klovrp_overlays_publish(const kl_ovrp_overlay *v, int n);
static int klovrp_enq_with_xr(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_ENQ_WITH_XR", t && (!strcmp(t, "x2094303753986147") ||
                                                    !strcmp(t, "x6946182742093178") ||
                                                    !strcmp(t, "x24151641901194680")));
        if (v) fprintf(stderr, "  [ovrp] enqueued panels ride on the OpenXR frame "
                               "(KL_OVRP_ENQ_WITH_XR)\n");
    }
    return v;
}
static pthread_mutex_t g_enq_xr_mu = PTHREAD_MUTEX_INITIALIZER;
static kl_ovrp_overlay g_enq_xr[KLOVRP_MAX_LAYERS];
static int g_enq_xr_n;
static volatile int g_xr_files_overlays;   // an OpenXR frame has filed its quads
void kl_ovrp_overlays_external(const kl_ovrp_overlay *v, int n) {
    if (!klovrp_enq_with_xr()) { klovrp_overlays_publish(v, n); return; }
    __atomic_store_n(&g_xr_files_overlays, 1, __ATOMIC_RELAXED);
    kl_ovrp_overlay all[KLOVRP_MAX_LAYERS];
    int k = 0;
    for (int i = 0; v && i < n && k < KLOVRP_MAX_LAYERS; i++) all[k++] = v[i];
    pthread_mutex_lock(&g_enq_xr_mu);
    for (int i = 0; i < g_enq_xr_n && k < KLOVRP_MAX_LAYERS; i++) all[k++] = g_enq_xr[i];
    pthread_mutex_unlock(&g_enq_xr_mu);
    klovrp_overlays_publish(all, k);
}
// The enqueue path's set. Without KL_OVRP_ENQ_WITH_XR, or before an OpenXR
// frame has filed anything, it is published directly as before.
static void klovrp_enq_publish(const kl_ovrp_overlay *v, int n) {
    if (klovrp_enq_with_xr()) {
        if (n > KLOVRP_MAX_LAYERS) n = KLOVRP_MAX_LAYERS;
        if (n < 0 || !v) n = 0;
        pthread_mutex_lock(&g_enq_xr_mu);
        if (n) memcpy(g_enq_xr, v, (size_t)n * sizeof *v);
        g_enq_xr_n = n;
        pthread_mutex_unlock(&g_enq_xr_mu);
        if (__atomic_load_n(&g_xr_files_overlays, __ATOMIC_RELAXED)) return;
    }
    klovrp_overlays_publish(v, n);
}
static void klovrp_overlays_publish(const kl_ovrp_overlay *v, int n) {
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

// ...and what is BEHIND the quads of such a frame. On a Quest it is black: the
// eye buffer is simply absent. Here the space is a mixed one, and what the
// system shows where the app wrote no depth is the room — so with the eye pass
// off, Asgard's Wrath 2's loading screen was its picture in a ragged black
// frame with the player's living room around it ("outside it is no longer
// black but passthrough, that is wrong and looks broken", 2026-10-11). While
// this is set the compositor puts black behind the frame's quads. Only the
// OVRPlugin switch above sets it; an OpenXR guest's quad-only frames are left
// as they were.
static int g_eyeless_black;
void kl_ovrp_set_eyeless_black(int on) { g_eyeless_black = !!on; }
int  kl_ovrp_eyeless_black(void) { return g_eyeless_black && !g_eye_layer_live; }

// ---------------------------------------------------------------------------
// Insight Passthrough — the guest's own mixed reality
// ---------------------------------------------------------------------------
// A Quest game asks for passthrough (ovrp_InitializeInsightPassthrough), sets
// up a layer of a passthrough shape and submits it UNDER its eye layer; the
// headset then shows the cameras' picture wherever the eye picture is
// transparent. Nothing of that picture has to be made here: every game runs in
// a mixed space, and the room is what the system shows wherever the composite
// is transparent. So the passthrough layer itself is never drawn. While the
// guest submits one,
//   - the eye pass stops forcing the eye picture opaque and reads its alpha
//     (kl_reproject_set_eye_alpha), and
//   - the compositor leaves out the black it otherwise puts behind a frame
//     that has no picture (kl_ovrp_passthrough_live, KleptonCompositor).
// Without a passthrough layer in the list everything is as it was.
//
// Until this, every guest was told there is no passthrough (UNSUPPORTED, an
// empty capability mask), and a game stayed in its ordinary mode. That is
// still the answer wherever this is off.
//   KL_OVRP_PASSTHROUGH=0|1 — default 1 for TRIANGLE STRATEGY
//       (x5340130476095884), whose mixed-reality mode this was written for
//       (2026-10-12), for Demeo (x3634830803298285), for Richie's Plank
//       Experience (x1642239225880682), for Synth Riders
//       (x2436558143118760: its MR mode "macht nichts", 2026-10-15) and for
//       LES MILLS BODYCOMBAT (x4015163475201433), else 0: no other
//       game has been seen with a "yes".
//
// A Unity guest (OVROverlay / OVRPassthroughLayer) does not put the layer in
// ovrp_EndFrame4's list: it sets it up through ovrp_EnqueueSetupLayer2 and
// files it every frame through ovrp_EnqueueSubmitLayer2. Both ways count.
//   KL_OVRP_PT_ALPHA=opaque|coverage|inverse — what the eye picture's alpha
//       means while a passthrough layer is submitted. Default: read from the
//       eye submit's flags — ovrpLayerSubmitFlag_InverseAlpha, which Unreal's
//       Oculus plugin sets on Android when layers lie under the eye layer —
//       and coverage without it.
static int klovrp_pt_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_OVRP_PASSTHROUGH", t && (!strcmp(t, "x5340130476095884") ||
                                                    !strcmp(t, "x3634830803298285") ||
                                                    !strcmp(t, "x1642239225880682") ||
                                                    !strcmp(t, "x2436558143118760") ||
                                                    !strcmp(t, "x4015163475201433") ||
                                                    !strcmp(t, "x9449316705178989") ||
                                                    // Cubism's puzzle background can be
                                                    // replaced by the room via its MR mode.
                                                    !strcmp(t, "x2264524423619421") ||
                                                    // LEGO Bricktales has an AR mode; asked
                                                    // for by the owner (2026-10-17).
                                                    !strcmp(t, "x6521909757843713") ||
                                                    // Angry Birds VR: Isle of Pigs has a
                                                    // mixed reality mode; asked for by the
                                                    // owner (2026-10-18).
                                                    !strcmp(t, "x2718606324833775") ||
                                                    // Spatial Zombies is mixed reality
                                                    // only; without it the room was black
                                                    // (2026-10-28).
                                                    !strcmp(t, "x24151641901194680")));
        if (on)
            fprintf(stderr, "  [ovrp] passthrough: offered to the guest — the room shows "
                            "through the eye picture while it submits a passthrough layer\n");
    }
    return on;
}

// As far as the public header is remembered, and consistent with the 0x8
// (NoDepth) every Unreal panel in the corpus submits: HeadLocked 1<<0,
// ReverseZ 1<<2, NoDepth 1<<3, InverseAlpha 1<<4.
#define KLOVRP_SUBMIT_INVERSE_ALPHA 0x10

static int g_pt_init;                 // between Initialize- and ShutdownInsightPassthrough
// EndFrames since a passthrough layer was last in the list. A few frames of
// grace, so a list that leaves it out once does not flash black.
#define KLOVRP_PT_GRACE_FRAMES 6
static volatile unsigned g_pt_frames_since = 1000000u;
int kl_ovrp_passthrough_live(void) { return g_pt_frames_since < KLOVRP_PT_GRACE_FRAMES; }
// EndFrames since a passthrough layer last came through ovrp_EnqueueSubmitLayer2,
// and its shape. Filed on the guest's main thread, read on the one that ends
// frames; a frame's difference between the two is inside the grace above.
static volatile unsigned g_pt_enq_since = 1000000u;
static volatile int g_pt_enq_shape;
static void klovrp_pt_enqueued(int shape) { g_pt_enq_shape = shape; g_pt_enq_since = 0; }
// The layer PassthroughLayerResumed was last told for (see klovrp_PollEvent2);
// forgotten when the guest shuts passthrough down, so turning it on again is
// told again.
static int g_pt_resumed_layer = -1;
//   KL_OVRP_PT_RESUMED=0|1 — default 1 for Synth Riders (x2436558143118760)
//       only: the games passthrough already worked for were seen without it.
static int klovrp_pt_resumed_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_OVRP_PT_RESUMED", t && !strcmp(t, "x2436558143118760"));
    }
    return on;
}
static void klovrp_pt_resumed(int layer_id) {
    if (!klovrp_pt_resumed_on()) return;
    if (!g_pt_init || layer_id == g_pt_resumed_layer) return;
    g_pt_resumed_layer = layer_id;
    int32_t data = layer_id;                   // PassthroughLayerResumedData { int LayerId }
    klovrp_event_push(KLOVRP_EVENT_PASSTHROUGH_LAYER_RESUMED, &data, sizeof data);
    fprintf(stderr, "  [ovrp] passthrough: PassthroughLayerResumed (layer %d) queued\n", layer_id);
}

// An OpenXR-backend guest (MetaXRFeature; DreamSpace, 2026-10-16) files its
// OVRPassthroughLayer through ovrp_EnqueueSubmitLayer2 every frame, exactly as
// a Unity guest on the OVRPlugin path does, but its frames end in kl_openxr's
// xrEndFrame and never reach ovrp_EndFrame4 — so klovrp_pt_submits never ran,
// passthrough never went live, and the guest's transparent background was
// composited over black. Here the eye picture is the OpenXR projection layer,
// whose alpha is the standard one (1 = the guest's picture): coverage, unless
// KL_OVRP_PT_ALPHA says otherwise. Only where passthrough is offered at all,
// and only once such a layer has been filed: an OpenXR guest that never files
// one keeps its eye alpha as it was.
void kl_ovrp_openxr_frame_end(void) {
    extern void kl_reproject_set_eye_alpha(int mode);
    // An OpenXR guest's frames are counted here, so a KL_OVRP_POKE press timed in frames (`f<n>`)
    // fires on one too: the same number its KL_VK_OUT captures carry (xr_f<n>_*.png). Before, the
    // counter only moved on ovrp_EndFrame and such a press never came (HITMAN 3 VR: Reloaded on the
    // Mac, 2026-10-28).
    __atomic_fetch_add(&g_vk_frame, 1, __ATOMIC_RELAXED);
    // Enqueued panels riding on OpenXR frames age on this boundary — see
    // kl_ovrp_overlays_external.
    if (klovrp_enq_with_xr()) klovrp_enq_age();
    // ...and on a GLES guest their images are copied here, where ovrp_EndFrame
    // would have (klovrp_mirror_enq_gl): this thread is the guest's render
    // thread with its context current. Without the copy every panel reached
    // the compositor with no MTLTexture and was dropped — Fluid
    // (x6946182742093178), whose whole UI is two such Quads, stayed black
    // (2026-10-21).
    // Spatial Zombies (x24151641901194680, GLES) is the same with its menu.
    //   KL_OVRP_ENQ_MIRROR_XR=0|1 — default 1 for Fluid and Spatial Zombies, else 0.
    static int mirror = -1;
    if (mirror < 0) {
        const char *t = kl_driver_target_name();
        mirror = kl_env_on("KL_OVRP_ENQ_MIRROR_XR", t && (!strcmp(t, "x6946182742093178") ||
                                                           !strcmp(t, "x24151641901194680")));
    }
    if (mirror && klovrp_enq_with_xr()) klovrp_mirror_enq_gl();
    if (!klovrp_pt_on() || g_pt_enq_since >= 1000000u) return;
    g_pt_enq_since++;
    int enqueued = g_pt_enq_since <= 3;
    int was_live = kl_ovrp_passthrough_live();
    if (enqueued) g_pt_frames_since = 0;
    else if (g_pt_frames_since < 1000000u) g_pt_frames_since++;
    int live = kl_ovrp_passthrough_live();
    static int forced = -2;
    if (forced == -2) {
        const char *e = kl_env_str("KL_OVRP_PT_ALPHA", NULL);
        forced = !e || !*e ? -1
               : (!strcmp(e, "opaque") || !strcmp(e, "0")) ? 0
               : (!strcmp(e, "inverse") || !strcmp(e, "2")) ? 2 : 1;
    }
    kl_reproject_set_eye_alpha(live ? (forced >= 0 ? forced : 1) : 0);
    if (live != was_live)
        fprintf(stderr, "  [ovrp] passthrough: %s (OpenXR frames; layer filed through "
                        "ovrp_EnqueueSubmitLayer2, shape %s) — eye alpha %s\n",
                live ? "LIVE" : "off", klovrp_shape_name(g_pt_enq_shape),
                !live ? "opaque" : forced == 0 ? "opaque" : forced == 2 ? "inverse" : "coverage");
}

static void klovrp_pt_submits(const void *layer_submits, int count) {
    extern void kl_reproject_set_eye_alpha(int mode);
    if (!klovrp_pt_on()) return;
    const ovrp_layer_submit *const *list = layer_submits;
    int pt = -1, eye = -1, shape = 0, eye_flags = 0, eye_fmt = -1;
    for (int i = 0; list && i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l) continue;
        if (l->is_eye) {
            if (eye < 0) { eye = i; eye_flags = s->submit_flags; eye_fmt = l->desc.format; }
        } else if (pt < 0 && klovrp_shape_is_passthrough(l->desc.shape)) {
            pt = i; shape = l->desc.shape;
            klovrp_pt_resumed(s->layer_id);
        }
    }
    // ...or filed by itself, the way Unity does it. Not in the list, so it has
    // no place there: said to be under the eye layer, which is what an
    // OVRPassthroughLayer is unless its owner chose otherwise.
    int enqueued = 0;
    if (g_pt_enq_since < 1000000u) g_pt_enq_since++;
    if (pt < 0 && g_pt_enq_since <= 3) { enqueued = 1; shape = g_pt_enq_shape; }
    int was_live = kl_ovrp_passthrough_live();
    if (pt >= 0 || enqueued) g_pt_frames_since = 0;
    else if (g_pt_frames_since < 1000000u) g_pt_frames_since++;
    int live = kl_ovrp_passthrough_live();

    // 0 opaque, 1 coverage, 2 inverse — KL_EYE_ALPHA_* in kl_reproject.h.
    static int forced = -2, mode, said_mode = -1;
    if (forced == -2) {
        const char *e = kl_env_str("KL_OVRP_PT_ALPHA", NULL);
        forced = !e || !*e ? -1
               : (!strcmp(e, "opaque") || !strcmp(e, "0")) ? 0
               : (!strcmp(e, "inverse") || !strcmp(e, "2")) ? 2 : 1;
    }
    if (!live) {
        mode = 0;
    } else if ((pt >= 0 || enqueued) && eye >= 0) {
        // An eye format with no alpha channel samples as 1 everywhere, which
        // read inversely would be a picture that is not there at all.
        int no_alpha = eye_fmt == 3 || eye_fmt == 6 || eye_fmt == 7;
        mode = no_alpha ? 0
             : forced >= 0 ? forced
             : (eye_flags & KLOVRP_SUBMIT_INVERSE_ALPHA) ? 2 : 1;
        if (mode != said_mode) {
            said_mode = mode;
            static const char *const M[] = {
                "left opaque", "read as coverage (1 = the game's picture)",
                "read inversely (0 = the game's picture)" };
            char where[64];
            if (enqueued) snprintf(where, sizeof where, "filed through ovrp_EnqueueSubmitLayer2");
            else snprintf(where, sizeof where, "submitted %s the eye layer (%d and %d of %d)",
                          pt < eye ? "under" : "OVER", pt + 1, eye + 1, count);
            fprintf(stderr, "  [ovrp] passthrough: a %s layer is %s — the eye picture's "
                            "alpha is %s; eye submit flags %#x, format %d%s%s\n",
                    klovrp_shape_name(shape), where, M[mode], (unsigned)eye_flags, eye_fmt,
                    no_alpha ? " (no alpha channel)" : "",
                    forced >= 0 ? " (KL_OVRP_PT_ALPHA)" : "");
        }
    }
    kl_reproject_set_eye_alpha(mode);
    if (live != was_live) {
        if (!live) said_mode = -1;
        fprintf(stderr, "  [ovrp] passthrough: %s\n",
                live ? "the guest's frames carry a passthrough layer — no black behind "
                       "them, the room instead"
                     : "no passthrough layer for a few frames — the eye picture is "
                       "opaque again, black behind frames without one");
    }
}

// The calls around it. Each is the old answer while passthrough is off.
static uint64_t klovrp_InitializeInsightPassthrough(void) {
    ovrp_hit("ovrp_InitializeInsightPassthrough");
    if (!klovrp_pt_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!g_pt_init) fprintf(stderr, "  [ovrp] passthrough: initialised by the guest\n");
    g_pt_init = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_ShutdownInsightPassthrough(void) {
    ovrp_hit("ovrp_ShutdownInsightPassthrough");
    if (g_pt_init) fprintf(stderr, "  [ovrp] passthrough: shut down by the guest\n");
    g_pt_init = 0;
    g_pt_resumed_layer = -1;
    return OVRP_SUCCESS;
}

// ---- An emulated room: the scene a Quest's Space Setup would have recorded ----
//
// Synth Riders' MR button (com.Kluge.XR.MixedReality.MixedRealityManager, read
// off its Cpp2IL dump on 2026-10-15) turns passthrough on and then calls
// OVRSceneManager.LoadSceneModel; only OnSceneModelLoadedSuccessfully shows the
// room — it takes the floor's outline (GetSpaceBoundary2D) for its play area and
// sorts the walls by direction — and with no room OnNoSceneModelToLoad asks for
// Space Setup and turns MR off again. This plugin had no rooms at all
// (ovrp_DiscoverSpaces: UNSUPPORTED), so the button "did nothing".
//
// What is answered here is a room as the scene API describes one: a room anchor
// (RoomLayout, SpaceContainer, Locatable) holding a floor, a ceiling and four
// walls (Locatable, Bounded2D, SemanticLabels). By default a box around the
// head (see KL_OVRP_SCENE_PLANES, kl_ovrp_scene_wanted). With the switch on,
// the room is the real one where visionOS has seen it: the frontend's plane detection (KleptonCompositor, a
// session of its own) hands floor, ceiling and wall planes over through
// kl_ovrp_scene_planes, in ARKit's world space — the space the head pose comes
// in — and they are shifted here exactly as the head is (EyeLevel height, stick
// movement). What it has not seen is filled in: no floor, one at y=0 under the
// walls or the head; no ceiling, one 2.6 m above the floor; no walls at all, the
// four of a box around the floor. Taken when the guest asks for rooms
// (DiscoverSpaces with a RoomLayout filter) and kept until it asks again.
//
// Plane frames follow the scene convention: the plane in local XY, +Z its
// normal — up for the floor, down for the ceiling, into the room for a wall.
//   KL_OVRP_SCENE_ROOM=0|1 — default 1 for Synth Riders (x2436558143118760),
//       DreamSpace (x9449316705178989) and Spatial Zombies (x24151641901194680),
//       else 0, where every one of these says UNSUPPORTED as before.
//   KL_OVRP_ROOM=<w>x<d>x<h> — the box in metres, default 4x4x2.6, where no
//       plane has been seen.
static int klovrp_scene_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        // DreamSpace (x9449316705178989) is a mixed-reality home built on rooms.
        // Spatial Zombies (x24151641901194680): MR Utility Kit on Unity's OpenXR
        // plugin, which asked for xrDiscoverSpacesMETA ~3700 times a run, was
        // told it is not there, and never left its empty start scene (2026-10-28).
        on = kl_env_on("KL_OVRP_SCENE_ROOM", t && (!strcmp(t, "x2436558143118760") ||
                                                   !strcmp(t, "x9449316705178989") ||
                                                   !strcmp(t, "x24151641901194680")));
        if (on) fprintf(stderr, "  [ovrp] scene: an emulated room is offered (KL_OVRP_SCENE_ROOM)\n");
    }
    return on;
}

// KL_OVRP_SCENE_PLANES=0|1 — build the room from visionOS's plane detection
// (default 0). Marcel on the measured room, 2026-10-15: "Der Raum ist
// verschoben, dann lieber mit dem fake Raum ... das ist stabiler" — it came
// out shifted, and the first press found nothing until he had looked around.
// Off, the box around the head is the room, at once and always the same.
int kl_ovrp_scene_wanted(void) {
    static int on = -1;
    if (on < 0) on = klovrp_scene_on() && kl_env_on("KL_OVRP_SCENE_PLANES", 0);
    return on;
}

enum { KLR_ROOM, KLR_FLOOR, KLR_CEILING, KLR_WALL0, KLR_N = KLR_WALL0 + 12 };   // KLR_N: the most
#define KLR_HANDLE0 0x6b6c00000000a000ull      // handles: KLR_HANDLE0 + index
typedef struct { float qx, qy, qz, qw, px, py, pz; } klr_posef;   // ovrpPosef: orientation, then position
static struct {
    int built, n;               // n anchors: room, floor, ceiling, then walls
    float w, d, h;              // extents
    klr_posef pose[KLR_N];
    float ext_x[KLR_N], ext_y[KLR_N];   // plane size in its local X and Y
    int locatable_on[KLR_N];
} g_room;

static void klr_uuid(int i, unsigned char out[16]) {
    // Fixed, recognisable and distinct: "klepton-room" + index.
    static const unsigned char base[16] = { 'k','l','e','p','t','o','n','-','r','o','o','m',0,0,0,0 };
    memcpy(out, base, 16);
    out[15] = (unsigned char)(i + 1);
    out[6] = (unsigned char)((out[6] & 0x0f) | 0x40);   // version 4, variant 1: a well-formed GUID
    out[8] = (unsigned char)((out[8] & 0x3f) | 0x80);
}
static int klr_index(uint64_t handle) {
    if (handle < KLR_HANDLE0 || handle >= KLR_HANDLE0 + (uint64_t)g_room.n) return -1;
    return (int)(handle - KLR_HANDLE0);
}
static int klr_index_of_uuid(const unsigned char u[16]) {
    unsigned char t[16];
    for (int i = 0; i < g_room.n; i++) { klr_uuid(i, t); if (!memcmp(t, u, 16)) return i; }
    return -1;
}
// The planes the frontend has seen, ARKit world space: kind 0 floor, 1 ceiling,
// 2 wall; m the scene-convention pose (column-major 4x4: plane in local XY,
// +Z its normal), ext its width and height.
#define KLR_SEEN_MAX 24
struct klr_seen { int kind; float m[16]; float w, h; };
static struct klr_seen g_klr_seen[KLR_SEEN_MAX];
static int g_klr_seen_n;
// The room the start window's scan measured, as the frontend placed it in this
// world (KleptonBoundary, kl_room_box_planes): floor, ceiling and four walls
// that close. Preferred to plane detection and to the box where it is there
// (KL_OVRP_SCENE_MEASURED=0 leaves it out).
static struct klr_seen g_klr_meas[KLR_SEEN_MAX];
static int g_klr_meas_n;
static pthread_mutex_t g_klr_seen_mu = PTHREAD_MUTEX_INITIALIZER;

void kl_ovrp_scene_room_planes(int count, const int *kinds, const float *m16, const float *ext2) {
    if (count < 0) count = 0;
    if (count > KLR_SEEN_MAX) count = KLR_SEEN_MAX;
    pthread_mutex_lock(&g_klr_seen_mu);
    for (int i = 0; i < count; i++) {
        g_klr_meas[i].kind = kinds[i];
        memcpy(g_klr_meas[i].m, m16 + 16 * i, sizeof g_klr_meas[i].m);
        g_klr_meas[i].w = ext2[2 * i]; g_klr_meas[i].h = ext2[2 * i + 1];
    }
    g_klr_meas_n = count;
    pthread_mutex_unlock(&g_klr_seen_mu);
    static int said;
    if (said++ < 8) {
        float fw = 0, fd = 0, h = 0;
        for (int i = 0; i < count; i++) {
            if (kinds[i] == 0) { fw = ext2[2 * i]; fd = ext2[2 * i + 1]; }
            if (kinds[i] == 2 && ext2[2 * i + 1] > h) h = ext2[2 * i + 1];
        }
        fprintf(stderr, "  [ovrp] scene: the measured room is handed over — %.2f x %.2f m, %.2f m high, %d planes "
                        "(used the next time the guest asks for rooms)\n", (double)fw, (double)fd, (double)h, count);
    }
}

void kl_ovrp_scene_planes(int count, const int *kinds, const float *m16, const float *ext2) {
    if (count < 0) count = 0;
    if (count > KLR_SEEN_MAX) count = KLR_SEEN_MAX;
    pthread_mutex_lock(&g_klr_seen_mu);
    for (int i = 0; i < count; i++) {
        g_klr_seen[i].kind = kinds[i];
        memcpy(g_klr_seen[i].m, m16 + 16 * i, sizeof g_klr_seen[i].m);
        g_klr_seen[i].w = ext2[2 * i]; g_klr_seen[i].h = ext2[2 * i + 1];
    }
    int was = g_klr_seen_n;
    g_klr_seen_n = count;
    pthread_mutex_unlock(&g_klr_seen_mu);
    static int said;
    if (count != was && said++ < 12) {
        int f = 0, c = 0, w = 0;
        for (int i = 0; i < count; i++) { if (kinds[i] == 0) f++; else if (kinds[i] == 1) c++; else w++; }
        fprintf(stderr, "  [ovrp] scene: visionOS sees %d floor, %d ceiling, %d wall plane(s)\n", f, c, w);
    }
}

// A 3x3 rotation (column-major, from a 4x4) to a quaternion.
static void klr_quat_from_m(const float *m, float *qx, float *qy, float *qz, float *qw) {
    float r00 = m[0], r10 = m[1], r20 = m[2], r01 = m[4], r11 = m[5], r21 = m[6], r02 = m[8], r12 = m[9], r22 = m[10];
    float t = r00 + r11 + r22, x, y, z, w;
    if (t > 0) { float S = sqrtf(t + 1.0f) * 2; w = 0.25f * S; x = (r21 - r12) / S; y = (r02 - r20) / S; z = (r10 - r01) / S; }
    else if (r00 > r11 && r00 > r22) { float S = sqrtf(1.0f + r00 - r11 - r22) * 2; w = (r21 - r12) / S; x = 0.25f * S; y = (r01 + r10) / S; z = (r02 + r20) / S; }
    else if (r11 > r22) { float S = sqrtf(1.0f + r11 - r00 - r22) * 2; w = (r02 - r20) / S; x = (r01 + r10) / S; y = 0.25f * S; z = (r12 + r21) / S; }
    else { float S = sqrtf(1.0f + r22 - r00 - r11) * 2; w = (r10 - r01) / S; x = (r02 + r20) / S; y = (r12 + r21) / S; z = 0.25f * S; }
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n > 0) { x /= n; y /= n; z /= n; w /= n; }
    *qx = x; *qy = y; *qz = z; *qw = w;
}

static void klr_build(void) {
    float bw = 4.0f, bd = 4.0f, bh = 2.6f;
    const char *e = kl_env_str("KL_OVRP_ROOM", NULL);
    if (e && *e) {
        float a, b, c;
        if (sscanf(e, "%fx%fx%f", &a, &b, &c) == 3 && a > 0.5f && b > 0.5f && c > 1.0f) { bw = a; bd = b; bh = c; }
    }
    const float s = 0.70710678f;
    pthread_mutex_lock(&g_klr_seen_mu);
    // The measured room first (kl_ovrp_scene_room_planes): a closed box from
    // the start window's scan, placed in this world. Then plane detection.
    int measured = g_klr_meas_n > 0 && kl_env_on("KL_OVRP_SCENE_MEASURED", 1);
    const struct klr_seen *src = measured ? g_klr_meas : g_klr_seen;
    int src_n = measured ? g_klr_meas_n : g_klr_seen_n;
    int floor_i = -1, ceil_i = -1;
    float floor_a = 0, ceil_a = 0;
    for (int i = 0; i < src_n; i++) {
        float a = src[i].w * src[i].h;
        if (src[i].kind == 0 && a > floor_a) { floor_a = a; floor_i = i; }
        if (src[i].kind == 1 && a > ceil_a)  { ceil_a = a;  ceil_i = i; }
    }
    int n = KLR_WALL0;
    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (int i = 0; i < src_n && n < KLR_N; i++) {
        if (src[i].kind != 2 || src[i].w * src[i].h < 0.3f) continue;
        const float *m = src[i].m;
        klr_quat_from_m(m, &g_room.pose[n].qx, &g_room.pose[n].qy, &g_room.pose[n].qz, &g_room.pose[n].qw);
        g_room.pose[n].px = m[12]; g_room.pose[n].py = m[13]; g_room.pose[n].pz = m[14];
        g_room.ext_x[n] = src[i].w; g_room.ext_y[n] = src[i].h;
        if (m[12] < minx) minx = m[12]; if (m[12] > maxx) maxx = m[12];
        if (m[14] < minz) minz = m[14]; if (m[14] > maxz) maxz = m[14];
        n++;
    }
    int walls_seen = n - KLR_WALL0;
    // The floor: seen, or at y=0 under the walls' middle (or the head).
    float cx = g_head_set ? g_head_pose.px : 0.0f, cz = g_head_set ? g_head_pose.pz : 0.0f;
    if (walls_seen >= 2) { cx = (minx + maxx) / 2; cz = (minz + maxz) / 2; }
    float fy = 0.0f;
    if (floor_i >= 0) {
        const float *m = src[floor_i].m;
        klr_quat_from_m(m, &g_room.pose[KLR_FLOOR].qx, &g_room.pose[KLR_FLOOR].qy,
                        &g_room.pose[KLR_FLOOR].qz, &g_room.pose[KLR_FLOOR].qw);
        g_room.pose[KLR_FLOOR].px = m[12]; g_room.pose[KLR_FLOOR].py = fy = m[13]; g_room.pose[KLR_FLOOR].pz = m[14];
        g_room.ext_x[KLR_FLOOR] = src[floor_i].w; g_room.ext_y[KLR_FLOOR] = src[floor_i].h;
    } else {
        float w = walls_seen >= 2 ? fmaxf(maxx - minx, 1.0f) : bw, d = walls_seen >= 2 ? fmaxf(maxz - minz, 1.0f) : bd;
        g_room.pose[KLR_FLOOR] = (klr_posef){ -s, 0, 0, s, cx, 0, cz };     // +Z -> +Y
        g_room.ext_x[KLR_FLOOR] = w; g_room.ext_y[KLR_FLOOR] = d;
    }
    if (ceil_i >= 0) {
        const float *m = src[ceil_i].m;
        klr_quat_from_m(m, &g_room.pose[KLR_CEILING].qx, &g_room.pose[KLR_CEILING].qy,
                        &g_room.pose[KLR_CEILING].qz, &g_room.pose[KLR_CEILING].qw);
        g_room.pose[KLR_CEILING].px = m[12]; g_room.pose[KLR_CEILING].py = m[13]; g_room.pose[KLR_CEILING].pz = m[14];
        g_room.ext_x[KLR_CEILING] = src[ceil_i].w; g_room.ext_y[KLR_CEILING] = src[ceil_i].h;
    } else {
        g_room.pose[KLR_CEILING] = (klr_posef){ s, 0, 0, s, g_room.pose[KLR_FLOOR].px, fy + bh, g_room.pose[KLR_FLOOR].pz };
        g_room.ext_x[KLR_CEILING] = g_room.ext_x[KLR_FLOOR]; g_room.ext_y[KLR_CEILING] = g_room.ext_y[KLR_FLOOR];
    }
    int seen_n = src_n;
    pthread_mutex_unlock(&g_klr_seen_mu);
    if (walls_seen == 0) {
        // The four walls of a box around the floor, normals inward.
        float px = g_room.pose[KLR_FLOOR].px, pz = g_room.pose[KLR_FLOOR].pz;
        float w = g_room.ext_x[KLR_FLOOR], d = g_room.ext_y[KLR_FLOOR], h = bh;
        if (floor_i >= 0) { w = fmaxf(w, 2.0f); d = fmaxf(d, 2.0f); }
        g_room.pose[KLR_WALL0 + 0] = (klr_posef){ 0, 0, 0, 1,  px, fy + h / 2, pz - d / 2 };
        g_room.pose[KLR_WALL0 + 1] = (klr_posef){ 0, 1, 0, 0,  px, fy + h / 2, pz + d / 2 };
        g_room.pose[KLR_WALL0 + 2] = (klr_posef){ 0, s, 0, s,  px - w / 2, fy + h / 2, pz };
        g_room.pose[KLR_WALL0 + 3] = (klr_posef){ 0, -s, 0, s, px + w / 2, fy + h / 2, pz };
        g_room.ext_x[KLR_WALL0 + 0] = g_room.ext_x[KLR_WALL0 + 1] = w;
        g_room.ext_x[KLR_WALL0 + 2] = g_room.ext_x[KLR_WALL0 + 3] = d;
        for (int i = 0; i < 4; i++) g_room.ext_y[KLR_WALL0 + i] = h;
        n = KLR_WALL0 + 4;
    }
    g_room.pose[KLR_ROOM] = (klr_posef){ 0, 0, 0, 1, g_room.pose[KLR_FLOOR].px, fy, g_room.pose[KLR_FLOOR].pz };
    g_room.n = n;
    for (int i = 0; i < KLR_N; i++) g_room.locatable_on[i] = 0;
    g_room.built = 1;
    fprintf(stderr, "  [ovrp] scene: room from %d %s plane(s) — floor %s %.1f x %.1f m, ceiling %s, "
                    "%d wall(s)%s\n", seen_n, measured ? "measured-room" : "visionOS", floor_i >= 0 ? "seen" : "made",
            (double)g_room.ext_x[KLR_FLOOR], (double)g_room.ext_y[KLR_FLOOR],
            ceil_i >= 0 ? "seen" : "made", n - KLR_WALL0, walls_seen ? " seen" : " made (a box)");
}
// Which components an anchor has (SpaceComponentType numbers).
#define KLR_C_LOCATABLE 0
#define KLR_C_BOUNDED2D 3
#define KLR_C_BOUNDED3D 4
#define KLR_C_SEMANTIC  5
#define KLR_C_ROOMLAYOUT 6
#define KLR_C_CONTAINER 7
static int klr_has(int i, int comp) {
    if (comp == KLR_C_LOCATABLE) return 1;
    if (i == KLR_ROOM) return comp == KLR_C_ROOMLAYOUT || comp == KLR_C_CONTAINER;
    return comp == KLR_C_BOUNDED2D || comp == KLR_C_SEMANTIC;
}
static const char *klr_label(int i) {
    return i == KLR_FLOOR ? "FLOOR" : i == KLR_CEILING ? "CEILING" : i >= KLR_WALL0 ? "WALL_FACE" : "";
}

// Discovery requests: which anchors each one found, filed at ovrp_DiscoverSpaces
// and handed out at ovrp_RetrieveSpaceDiscoveryResults.
#define KLR_REQS 16
static struct { uint64_t id; int n; int idx[KLR_N]; } g_klr_req[KLR_REQS];
static uint64_t g_klr_next_req = 0x6b6c5200;
static pthread_mutex_t g_klr_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct { int32_t type; } klr_filter_hdr;
typedef struct { int32_t type; int32_t component; } klr_filter_comp;
typedef struct { int32_t type; int32_t n; const unsigned char *ids; } klr_filter_ids;
typedef struct { uint32_t n; const klr_filter_hdr *const *filters; } klr_discovery_info;

static uint64_t klovrp_DiscoverSpaces(const klr_discovery_info *info, uint64_t *request) {
    ovrp_hit("ovrp_DiscoverSpaces");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!info || !request) return OVRP_FAIL_INVALID_PARAM;
    // Asking for rooms is where a load starts: the room is taken anew from what
    // visionOS has seen by now, and held for the anchor queries that follow.
    int asks_rooms = 0;
    for (uint32_t f = 0; f < info->n && info->filters; f++) {
        const klr_filter_hdr *h = info->filters[f];
        if (h && h->type == 3 && ((const klr_filter_comp *)h)->component == KLR_C_ROOMLAYOUT) asks_rooms = 1;
    }
    if (asks_rooms || !g_room.built) klr_build();
    int pick[KLR_N];
    for (int i = 0; i < KLR_N; i++) pick[i] = i < g_room.n;
    // Filters narrow (AND): by component, or by a list of uuids.
    for (uint32_t f = 0; f < info->n && info->filters; f++) {
        const klr_filter_hdr *h = info->filters[f];
        if (!h) continue;
        if (h->type == 3) {
            int c = ((const klr_filter_comp *)h)->component;
            for (int i = 0; i < g_room.n; i++) if (!klr_has(i, c)) pick[i] = 0;
        } else if (h->type == 2) {
            const klr_filter_ids *fi = (const klr_filter_ids *)h;
            int want[KLR_N] = {0};
            for (int k = 0; k < fi->n && fi->ids; k++) {
                int i = klr_index_of_uuid(fi->ids + 16 * k);
                if (i >= 0) want[i] = 1;
            }
            for (int i = 0; i < KLR_N; i++) if (!want[i]) pick[i] = 0;
        }
    }
    pthread_mutex_lock(&g_klr_mu);
    uint64_t id = ++g_klr_next_req;
    int slot = (int)(id % KLR_REQS);
    g_klr_req[slot].id = id; g_klr_req[slot].n = 0;
    for (int i = 0; i < g_room.n; i++) if (pick[i]) g_klr_req[slot].idx[g_klr_req[slot].n++] = i;
    int n = g_klr_req[slot].n;
    pthread_mutex_unlock(&g_klr_mu);
    *request = id;
    fprintf(stderr, "  [ovrp] scene: DiscoverSpaces (%u filter(s)) -> request %#llx, %d anchor(s)\n",
            info->n, (unsigned long long)id, n);
    // SpaceDiscoveryResultsData { ulong RequestId } then
    // SpaceDiscoveryCompleteData { ulong RequestId; int Result }.
    unsigned char ev[16] = {0};
    memcpy(ev, &id, 8);
    if (n) klovrp_event_push(300u, ev, 8);
    int32_t ok = 0; memcpy(ev + 8, &ok, 4);
    klovrp_event_push(301u, ev, 12);
    return OVRP_SUCCESS;
}

typedef struct { uint64_t space; unsigned char uuid[16]; } klr_result;
typedef struct { uint32_t cap, count; klr_result *results; } klr_results;
static uint64_t klovrp_RetrieveSpaceDiscoveryResults(uint64_t request, klr_results *out) {
    ovrp_hit("ovrp_RetrieveSpaceDiscoveryResults");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    pthread_mutex_lock(&g_klr_mu);
    int slot = (int)(request % KLR_REQS);
    if (g_klr_req[slot].id != request) { pthread_mutex_unlock(&g_klr_mu); return OVRP_FAIL_INVALID_PARAM; }
    int n = g_klr_req[slot].n;
    out->count = (uint32_t)n;
    if (out->cap >= (uint32_t)n && out->results)
        for (int k = 0; k < n; k++) {
            int i = g_klr_req[slot].idx[k];
            out->results[k].space = KLR_HANDLE0 + (uint64_t)i;
            klr_uuid(i, out->results[k].uuid);
        }
    pthread_mutex_unlock(&g_klr_mu);
    if (out->cap && out->cap < (uint32_t)n) return (uint64_t)-1007;   // InsufficientSize
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetSpaceUuid(const uint64_t *space, unsigned char *uuid) {
    ovrp_hit("ovrp_GetSpaceUuid");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0 || !uuid) return (uint64_t)-1013;                         // HandleInvalid
    klr_uuid(i, uuid);
    return OVRP_SUCCESS;
}

static uint64_t klovrp_EnumerateSpaceSupportedComponents(const uint64_t *space, uint32_t cap,
                                                         uint32_t *count, int32_t *types) {
    ovrp_hit("ovrp_EnumerateSpaceSupportedComponents");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0 || !count) return (uint64_t)-1013;
    static const int32_t all[] = { KLR_C_LOCATABLE, KLR_C_BOUNDED2D, KLR_C_SEMANTIC, KLR_C_ROOMLAYOUT, KLR_C_CONTAINER };
    uint32_t n = 0;
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++)
        if (klr_has(i, all[k])) { if (types && n < cap) types[n] = all[k]; n++; }
    *count = n;
    return (cap && cap < n) ? (uint64_t)-1007 : OVRP_SUCCESS;
}

static uint64_t klovrp_GetSpaceComponentStatus(const uint64_t *space, int32_t comp,
                                               int32_t *enabled, int32_t *pending) {
    ovrp_hit("ovrp_GetSpaceComponentStatus");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0) return (uint64_t)-1013;
    if (!klr_has(i, comp)) return (uint64_t)-2005;                     // ComponentNotSupported
    if (enabled) *enabled = comp == KLR_C_LOCATABLE ? g_room.locatable_on[i] : 1;
    if (pending) *pending = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_SetSpaceComponentStatus(const uint64_t *space, int32_t comp, int32_t enable,
                                               double timeout, uint64_t *request) {
    ovrp_hit("ovrp_SetSpaceComponentStatus");
    (void)timeout;
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0) return (uint64_t)-1013;
    if (!klr_has(i, comp)) return (uint64_t)-2005;
    if (comp == KLR_C_LOCATABLE) g_room.locatable_on[i] = enable != 0;
    pthread_mutex_lock(&g_klr_mu);
    uint64_t id = ++g_klr_next_req;
    pthread_mutex_unlock(&g_klr_mu);
    if (request) *request = id;
    // SpaceSetComponentStatusCompleteData: RequestId 0x0, Result 0x8, Space 0x10,
    // Uuid 0x18, ComponentType 0x28, Enabled 0x2C.
    unsigned char ev[0x30] = {0};
    uint64_t h = *space; int32_t ok = 0, en = enable != 0;
    memcpy(ev + 0x00, &id, 8);
    memcpy(ev + 0x08, &ok, 4);
    memcpy(ev + 0x10, &h, 8);
    klr_uuid(i, ev + 0x18);
    memcpy(ev + 0x28, &comp, 4);
    memcpy(ev + 0x2C, &en, 4);
    klovrp_event_push(50u, ev, sizeof ev);
    return OVRP_SUCCESS;
}

// The pose in the tracking space the guest is given, shifted as the head is
// (kl_ovrp_set_head_pose): stick movement on x/z, the EyeLevel origin on y.
static klr_posef klr_tracked(int i) {
    klr_posef p = g_room.pose[i];
    klovrp_stick_move_offset(&p.px, &p.pz);
    p.py -= klovrp_origin_y();
    return p;
}
typedef struct { uint64_t flags; klr_posef pose; } klr_location;     // SpaceLocationf
static uint64_t klovrp_LocateSpace2(klr_location *out, const uint64_t *space, int32_t origin) {
    ovrp_hit("ovrp_LocateSpace2");
    (void)origin;
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0 || !out) return (uint64_t)-1013;
    out->flags = 0xF;                                                  // valid and tracked
    out->pose = klr_tracked(i);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_LocateSpace(klr_posef *out, const uint64_t *space, int32_t origin) {
    ovrp_hit("ovrp_LocateSpace");
    (void)origin;
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i < 0 || !out) return (uint64_t)-1013;
    *out = klr_tracked(i);
    return OVRP_SUCCESS;
}

typedef struct { int32_t cap, count; void *data; } klr_buf;            // the capacity/count/pointer idiom
static uint64_t klovrp_GetSpaceRoomLayout(const uint64_t *space, void *layout) {
    ovrp_hit("ovrp_GetSpaceRoomLayout");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!space || klr_index(*space) != KLR_ROOM || !layout) return (uint64_t)-2005;
    // RoomLayoutInternal: floorUuid 0x0, ceilingUuid 0x10, wall cap 0x20,
    // count 0x24, wallUuids 0x28.
    unsigned char *L = layout;
    klr_uuid(KLR_FLOOR, L);
    klr_uuid(KLR_CEILING, L + 0x10);
    klr_buf *walls = (klr_buf *)(L + 0x20);
    int nw = g_room.n - KLR_WALL0;
    walls->count = nw;
    if (walls->cap >= nw && walls->data)
        for (int k = 0; k < nw; k++) klr_uuid(KLR_WALL0 + k, (unsigned char *)walls->data + 16 * k);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSpaceContainer(const uint64_t *space, klr_buf *c) {
    ovrp_hit("ovrp_GetSpaceContainer");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!space || klr_index(*space) != KLR_ROOM || !c) return (uint64_t)-2005;
    c->count = g_room.n - 1;
    if (c->cap >= g_room.n - 1 && c->data)
        for (int k = 1; k < g_room.n; k++) klr_uuid(k, (unsigned char *)c->data + 16 * (k - 1));
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSpaceSemanticLabels(const uint64_t *space, klr_buf *b) {
    ovrp_hit("ovrp_GetSpaceSemanticLabels");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i <= KLR_ROOM || !b) return (uint64_t)-2005;
    const char *l = klr_label(i);
    int n = (int)strlen(l);
    b->count = n;
    if (b->cap >= n && b->data) {
        memcpy(b->data, l, (size_t)n);
        if (b->cap > n) ((char *)b->data)[n] = 0;
    }
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSpaceBoundingBox2D(const uint64_t *space, float *rect) {
    ovrp_hit("ovrp_GetSpaceBoundingBox2D");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i <= KLR_ROOM || !rect) return (uint64_t)-2005;
    rect[0] = -g_room.ext_x[i] / 2; rect[1] = -g_room.ext_y[i] / 2;    // Rectf: Pos, Size
    rect[2] = g_room.ext_x[i];      rect[3] = g_room.ext_y[i];
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSpaceBoundary2D(const uint64_t *space, klr_buf *b) {
    ovrp_hit("ovrp_GetSpaceBoundary2D");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    int i = space ? klr_index(*space) : -1;
    if (i <= KLR_ROOM || !b) return (uint64_t)-2005;
    // The rectangle, counter-clockwise seen along the normal.
    float x = g_room.ext_x[i] / 2, y = g_room.ext_y[i] / 2;
    const float v[8] = { -x, -y,  x, -y,  x, y,  -x, y };
    b->count = 4;
    if (b->cap >= 4 && b->data) memcpy(b->data, v, sizeof v);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetSpaceBoundingBox3D(const uint64_t *space, float *bounds) {
    ovrp_hit("ovrp_GetSpaceBoundingBox3D");
    (void)space; (void)bounds;
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    return (uint64_t)-2005;                                            // planes only
}
static uint64_t klovrp_DestroySpace(const uint64_t *space) {
    ovrp_hit("ovrp_DestroySpace");
    if (!klovrp_scene_on()) return OVRP_FAIL_UNSUPPORTED;
    return (space && klr_index(*space) >= 0) ? OVRP_SUCCESS : (uint64_t)-1013;
}
// The same room for OpenXR. A guest on Meta's OpenXR backend (MR Utility Kit:
// xrDiscoverSpacesMETA, xrGetSpaceRoomLayoutFB, ...) asks kl_openxr, not
// ovrp_*, and must be told about the SAME room — same anchors, same uuids, same
// poses — so kl_openxr serves those calls from here. Indices are the room's:
// 0 the room, 1 floor, 2 ceiling, 3.. walls. Component numbers are
// XrSpaceComponentTypeFB's, which are the ovrp ones (KLR_C_*).
int kl_ovrp_room_on(void) { return klovrp_scene_on(); }
int kl_ovrp_room_prepare(int rebuild) {
    if (!klovrp_scene_on()) return 0;
    if (rebuild || !g_room.built) klr_build();
    return g_room.n;
}
int kl_ovrp_room_count(void) { return g_room.built ? g_room.n : 0; }
void kl_ovrp_room_uuid(int i, unsigned char out[16]) { klr_uuid(i, out); }
int kl_ovrp_room_index_of_uuid(const unsigned char u[16]) { return klr_index_of_uuid(u); }
int kl_ovrp_room_has(int i, int comp) { return i >= 0 && i < g_room.n && klr_has(i, comp); }
const char *kl_ovrp_room_label(int i) { return klr_label(i); }
void kl_ovrp_room_extent(int i, float *x, float *y) {
    int ok = i >= 0 && i < KLR_N;
    *x = ok ? g_room.ext_x[i] : 0; *y = ok ? g_room.ext_y[i] : 0;
}
int kl_ovrp_room_locatable(int i) { return i >= 0 && i < KLR_N && g_room.locatable_on[i]; }
void kl_ovrp_room_set_locatable(int i, int on) { if (i >= 0 && i < KLR_N) g_room.locatable_on[i] = on != 0; }
// Orientation x y z w, then position — in the tracking space the guest's head
// is given in (klr_tracked), which is the space kl_openxr's STAGE is.
void kl_ovrp_room_pose(int i, float q[4], float p[3]) {
    klr_posef t = i >= 0 && i < KLR_N ? klr_tracked(i) : (klr_posef){ 0, 0, 0, 1, 0, 0, 0 };
    q[0] = t.qx; q[1] = t.qy; q[2] = t.qz; q[3] = t.qw;
    p[0] = t.px; p[1] = t.py; p[2] = t.pz;
}

// ovrpBool.
static uint64_t klovrp_GetInsightPassthroughInitialized(void) {
    ovrp_hit("ovrp_GetInsightPassthroughInitialized");
    return klovrp_pt_on() && g_pt_init;
}
// ovrpResult, and the result IS the state: success once initialised.
static uint64_t klovrp_GetInsightPassthroughInitializationState(void) {
    ovrp_hit("ovrp_GetInsightPassthroughInitializationState");
    if (!klovrp_pt_on()) return OVRP_FAIL_UNSUPPORTED;
    return g_pt_init ? OVRP_SUCCESS : OVRP_FAIL_NOT_INITIALIZED;
}
// (int layerId, ovrpInsightPassthroughStyle style) — the style is larger than
// two registers, so it arrives as a pointer to the caller's copy. Not read:
// opacity, edge colour and colour map are properties of a camera picture that
// is not drawn here.
static uint64_t klovrp_SetInsightPassthroughStyle(int layer_id, const void *style) {
    ovrp_hit("ovrp_SetInsightPassthroughStyle");
    (void)layer_id; (void)style;
    return klovrp_pt_on() ? OVRP_SUCCESS : OVRP_FAIL_UNSUPPORTED;
}
// Surface-projected passthrough: the guest's mesh, and its placements. Handles
// are handed out and nothing is kept — see klovrp_shape_is_passthrough.
//   ovrp_CreateInsightTriangleMesh(int layerId, float *vertices, int vertexCount,
//                                  int *triangles, int triangleCount, uint64 *mesh)
//   ovrp_AddInsightPassthroughSurfaceGeometry(int layerId, uint64 mesh,
//                                  ovrpMatrix4f T (by reference), uint64 *instance)
static uint64_t g_pt_next_handle = 1;
static uint64_t klovrp_CreateInsightTriangleMesh(int layer_id, const float *vertices,
                                                 int vertex_count, const int *triangles,
                                                 int triangle_count, uint64_t *mesh) {
    ovrp_hit("ovrp_CreateInsightTriangleMesh");
    (void)layer_id; (void)vertices; (void)vertex_count; (void)triangles; (void)triangle_count;
    if (!klovrp_pt_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!mesh) return OVRP_FAIL_INVALID_PARAM;
    *mesh = g_pt_next_handle++;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_AddInsightPassthroughSurfaceGeometry(int layer_id, uint64_t mesh,
                                                            const void *transform,
                                                            uint64_t *instance) {
    ovrp_hit("ovrp_AddInsightPassthroughSurfaceGeometry");
    (void)layer_id; (void)mesh; (void)transform;
    if (!klovrp_pt_on()) return OVRP_FAIL_UNSUPPORTED;
    if (!instance) return OVRP_FAIL_INVALID_PARAM;
    *instance = g_pt_next_handle++;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_pt_handle_call(const char *name) {
    ovrp_hit(name);
    return klovrp_pt_on() ? OVRP_SUCCESS : OVRP_FAIL_UNSUPPORTED;
}
static uint64_t klovrp_DestroyInsightTriangleMesh(uint64_t mesh) {
    (void)mesh; return klovrp_pt_handle_call("ovrp_DestroyInsightTriangleMesh");
}
static uint64_t klovrp_DestroyInsightPassthroughGeometryInstance(uint64_t instance) {
    (void)instance; return klovrp_pt_handle_call("ovrp_DestroyInsightPassthroughGeometryInstance");
}
static uint64_t klovrp_UpdateInsightPassthroughGeometryTransform(uint64_t instance,
                                                                 const void *transform) {
    (void)instance; (void)transform;
    return klovrp_pt_handle_call("ovrp_UpdateInsightPassthroughGeometryTransform");
}
// The newer questions around it, which a Unity guest asks once it is told a
// plugin version that has them (1.71, 1.84, 1.85, 1.87).
//   ovrp_IsInsightPassthroughSupported(ovrpBool *supported)
static uint64_t klovrp_IsInsightPassthroughSupported(int *out) {
    ovrp_hit("ovrp_IsInsightPassthroughSupported");
    if (out) *out = klovrp_pt_on() ? OVRP_TRUE : 0;
    return OVRP_SUCCESS;
}
// ovrpResult ovrp_IsControllerDrivenHandPosesEnabled(ovrpBool *enabled) — hands
// that are drawn from what the controllers do. Controllers here are controllers:
// not enabled. (Asked by Richie's Plank Experience once it is told 1.101.1.)
static uint64_t klovrp_IsControllerDrivenHandPosesEnabled(int *out) {
    ovrp_hit("ovrp_IsControllerDrivenHandPosesEnabled");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}
// ovrpInsightPassthroughCapabilities { Fields, Flags, MaxColorLutResolution } —
// the caller sets Fields to what it wants filled. Flags as in
// klovrp_GetPassthroughCapabilityFlags; no colour look-up tables (0), since
// there is no camera picture here to put through one.
static uint64_t klovrp_GetPassthroughCapabilities(int32_t *caps) {
    ovrp_hit("ovrp_GetPassthroughCapabilities");
    if (!caps) return OVRP_FAIL_INVALID_PARAM;
    caps[1] = klovrp_pt_on() ? 0x3 : 0;
    // MaxColorLutResolution: what the SDK reads as "colour LUTs supported".
    caps[2] = klovrp_pt_lut_on() ? KLOVRP_PT_LUT_MAX_RES : 0;
    return OVRP_SUCCESS;
}
// ovrpPassthroughPreferences { Flags } — bit 0 is "the user's home is in
// passthrough, start in it". Not claimed.
static uint64_t klovrp_GetPassthroughPreferences(int32_t *prefs) {
    ovrp_hit("ovrp_GetPassthroughPreferences");
    if (prefs) prefs[0] = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetInsightPassthroughStyle2(int layer_id, const void *style) {
    ovrp_hit("ovrp_SetInsightPassthroughStyle2");
    (void)layer_id; (void)style;
    return klovrp_pt_on() ? OVRP_SUCCESS : OVRP_FAIL_UNSUPPORTED;
}
// The guardian in a mixed-reality scene: ovrpBoundaryVisibility is
// NotSuppressed = 1, Suppressed = 2. There is no boundary drawn here at all,
// so it is suppressed, and asking for either is granted.
static uint64_t klovrp_GetBoundaryVisibility(int *out) {
    ovrp_hit("ovrp_GetBoundaryVisibility");
    if (out) *out = 2;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_RequestBoundaryVisibility(int visibility) {
    ovrp_hit("ovrp_RequestBoundaryVisibility");
    (void)visibility;
    return OVRP_SUCCESS;
}
// ovrpBool out-parameters that are "no" on this headset, with the call itself
// succeeding: local dimming (a Quest Pro panel), hands and controllers at once.
static uint64_t klovrp_GetLocalDimmingSupported(int *out) {
    ovrp_hit("ovrp_GetLocalDimmingSupported");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_IsMultimodalHandsControllersSupported(int *out) {
    ovrp_hit("ovrp_IsMultimodalHandsControllersSupported");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}
// ...and the eye layer's alpha being premultiplied, which is how it is read
// while a passthrough layer is submitted (kl_reproject.c): yes.
static uint64_t klovrp_GetEyeFovPremultipliedAlphaMode(int *out) {
    ovrp_hit("ovrp_GetEyeFovPremultipliedAlphaMode");
    if (out) *out = OVRP_TRUE;
    return OVRP_SUCCESS;
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
    // A world-locked layer's pose is in the guest's tracking space; a
    // head-locked one is relative to the head and has no height to convert.
    if (ok && !out->head_locked) out->pose[5] += klovrp_origin_y();
    return ok;
}

// ---- the eye picture's colour scale and offset ----------------------------
// What a Quest's compositor does to the eye picture after the game has drawn
// it: colour * scale + offset. It is how a game fades to black without drawing
// anything — OVRManager.SetColorScaleAndOffset, OVRScreenFade's compositor
// path, Unity.XR.Oculus.Utils.SetColorScale — and it was accepted and dropped
// here ("a fade shows its end state at once"). The end state of the fade a
// game holds while it LOADS is black, and dropping that shows the level being
// put together behind where the loading screen should be: Batman: Arkham
// Shadow ("you see the level build up while it loads, that was not so on the
// Quest", 2026-10-11).
//
// Handed to the reprojection (kl_reproject_set_eye_color), which is where both
// compositors get it from. The eye picture only: panels keep their own colour,
// so a loading icon stays lit over the black. KL_OVRP_COLOR_SCALE=0 drops the
// values again, as before.
typedef struct { float x, y, z, w; } klovrp_vec4;
static float g_color_scale[4] = { 1, 1, 1, 1 }, g_color_offset[4];
static int klovrp_color_sane(const float *v) {
    for (int i = 0; i < 4; i++) if (!(v[i] > -16.0f && v[i] < 16.0f)) return 0;   // NaN fails
    return 1;
}
static void klovrp_eye_color_apply(const float *scale, const float *offset, const char *from) {
    extern void kl_reproject_set_eye_color(const float scale[4], const float offset[4]);
    static float last_s[4] = { 1, 1, 1, 1 }, last_o[4];
    static unsigned said;
    if (!memcmp(last_s, scale, sizeof last_s) && !memcmp(last_o, offset, sizeof last_o)) return;
    memcpy(last_s, scale, sizeof last_s); memcpy(last_o, offset, sizeof last_o);
    kl_reproject_set_eye_color(scale, offset);
    // A fade is a few dozen steps; its ends are what a log is read for.
    int end = (scale[0] == 0 && scale[1] == 0 && scale[2] == 0) || (scale[0] == 1 && scale[1] == 1 && scale[2] == 1);
    if (said < 24 || (end && said < 400)) {
        said++;
        fprintf(stderr, "  [ovrp] eye colour scale (%.2f %.2f %.2f %.2f) offset (%.2f %.2f %.2f %.2f) — %s\n",
                scale[0], scale[1], scale[2], scale[3], offset[0], offset[1], offset[2], offset[3], from);
    }
}
// A plugin from before 1.55 ran on VrApi on a Quest, and VrApi's layers have a
// colour scale and no offset: the offset a game passed was dropped there. Catan
// VR (OVRPlugin 1.52) fades in with scale 0 -> 1 and an offset of 1 all along;
// applied, that is a white picture from the end of the splash on (2026-10-17).
// Such a game gets its offset dropped here as well. KL_OVRP_COLOR_OFFSET=1 keeps
// it, =0 drops it for every game.
static int klovrp_color_offset_wanted(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = klovrp_guest_version();
        int major = 0, minor = 0;
        int vrapi = v && sscanf(v, "%d.%d", &major, &minor) == 2 && major == 1 && minor < 55;
        on = kl_env_on("KL_OVRP_COLOR_OFFSET", !vrapi);
        if (!on)
            fprintf(stderr, "  [ovrp] colour offset dropped (OVRPlugin %s ran on VrApi on a Quest, "
                            "which has no offset; KL_OVRP_COLOR_OFFSET=1 keeps it)\n", v ? v : "?");
    }
    return on;
}
static uint64_t klovrp_SetColorScaleAndOffset(klovrp_vec4 scale, klovrp_vec4 offset, int all_layers) {
    ovrp_hit("ovrp_SetColorScaleAndOffset");
    (void)all_layers;        // the eye picture is what is faded here; panels keep their own colour
    float s[4] = { scale.x, scale.y, scale.z, scale.w }, o[4] = { offset.x, offset.y, offset.z, offset.w };
    if (!klovrp_color_offset_wanted()) memset(o, 0, sizeof o);
    if (!klovrp_color_sane(s) || !klovrp_color_sane(o)) return OVRP_SUCCESS;
    memcpy(g_color_scale, s, sizeof s); memcpy(g_color_offset, o, sizeof o);
    if (kl_env_on("KL_OVRP_COLOR_SCALE", 1)) klovrp_eye_color_apply(s, o, "ovrp_SetColorScaleAndOffset");
    return OVRP_SUCCESS;
}
// The eye layer's submit has a ColorScale/ColorOffset of its own (+0x48, +0x58)
// and a flag saying they override (+0xac). They are NOT applied: an older
// plugin's submit ends before them and those bytes are something else there,
// and no guest has been seen to need them. Said once, where a submit carries
// values that are not the identity, so that the game that does need them names
// itself in a log.
static void klovrp_submit_eye_color(const void *layer_submits, int count) {
    static int said;
    const ovrp_layer_submit *const *list = layer_submits;
    if (said || !list || count <= 0) return;
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l || !l->is_eye) continue;
        static const float one[4] = { 1, 1, 1, 1 }, zero[4];
        if (s->override_per_layer_color == 1 && klovrp_color_sane(s->color_scale) &&
            klovrp_color_sane(s->color_offset) &&
            (memcmp(s->color_scale, one, sizeof one) || memcmp(s->color_offset, zero, sizeof zero))) {
            said = 1;
            fprintf(stderr, "  [ovrp] the eye layer's submit overrides the colour scale itself: (%.2f %.2f %.2f %.2f) "
                            "offset (%.2f %.2f %.2f %.2f) — not applied (only ovrp_SetColorScaleAndOffset is)\n",
                    s->color_scale[0], s->color_scale[1], s->color_scale[2], s->color_scale[3],
                    s->color_offset[0], s->color_offset[1], s->color_offset[2], s->color_offset[3]);
        }
        return;
    }
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

// The GL Array eye layout's way onto the compositor (kl_ovrp_gl_array): the
// stage the guest names in its eye submit is an array texture of ours, and each
// of its two layers is copied into the provider-backed eye texture of that
// stage. With source 0 the stage IS the compositor's slot, which is the keying
// every other OVRPlugin eye uses. Runs on the thread that calls ovrp_EndFrame4 —
// the guest's own GL thread, where its context is current.
static void klovrp_mirror_array_eyes(const void *layer_submits, int count) {
    const ovrp_layer_submit *const *list = layer_submits;
    if (!list || count <= 0 || kl_vulkan_guest_active()) return;
    for (int i = 0; i < count; i++) {
        const ovrp_layer_submit *s = list[i];
        if (!s) continue;
        struct klovrp_layer *l = klovrp_layer(s->layer_id);
        if (!l || !l->is_eye || l->desc.layout != KLOVRP_LAYOUT_ARRAY) continue;
        int stage = s->texture_stage;
        if ((unsigned)stage >= KLOVRP_MAX_STAGES || !l->tex[stage][0]) return;
        const char *fname = NULL;
        uint32_t glfmt = klovrp_gl_format(l->desc.format, &fname);
        if (!glfmt) glfmt = KL_OVRP_TEXFMT_EYE;
        int w = l->desc.texture_size.w, h = l->desc.texture_size.h;
        for (int eye = 0; eye < 2; eye++) {
            int slot = kl_glfb_mirror_eye_layer(eye, stage, 0, l->tex[stage][0], eye, w, h, glfmt);
            static int said[2];
            if (slot < 0 && !said[0]++)
                fprintf(stderr, "  [ovrp] array eye %d stage %d could not be copied to the "
                                "compositor — that eye stays as it was\n", eye, stage);
            else if (slot >= 0 && !said[1]++)
                fprintf(stderr, "  [ovrp] array eyes: stage %d of GL array texture %u copied a "
                                "layer per eye into the compositor's slot %d (%dx%d)\n",
                        stage, l->tex[stage][0], slot, w, h);
        }
        // The copies are in ANGLE's stream; the record that points the compositor
        // at this stage is published right after. Order the two.
        kl_glfb_frame_fence();
        // KL_OVRP_EYE_DUMP=<seconds>: photograph both layers of the stage the guest
        // just submitted, every that many seconds, into <files>/eyedump — what the
        // guest drew, before any of ours touches it.
        {
            static int every = -2;
            static time_t next_at;
            static unsigned seq;
            if (every == -2) every = kl_env_int("KL_OVRP_EYE_DUMP", 0);
            time_t now = time(NULL);
            if (every > 0 && now >= next_at && seq < 6) {
                extern const char *kl_jni_files_dir(void);
                const char *base = kl_jni_files_dir();
                char dir[600], path[700];
                snprintf(dir, sizeof dir, "%s/eyedump", base ? base : ".");
                mkdir(dir, 0755);
                next_at = now + every;
                for (int eye = 0; eye < 2; eye++) {
                    snprintf(path, sizeof path, "%s/eye%d_%03u.png", dir, eye, seq);
                    int r = kl_glfb_dump_tex(path, l->tex[stage][0], eye, w, h);
                    fprintf(stderr, "  [ovrp] eye dump #%03u eye %d stage %d -> %s (%d)\n",
                            seq, eye, stage, path, r);
                }
                seq++;
            }
        }
        return;
    }
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
        // The rect goes into the frame record as the guest submitted it,
        // counted from the picture's first row. For a Vulkan guest that row is
        // the TOP, and the compositors mirror a rect shorter than the texture
        // themselves (kl_reproject_viewport_for_pass, since toolchain 478).
        //
        // Before that, this function did the mirroring for every Unreal guest
        // on Vulkan under a store-built target (Alien: Rogue Incursion,
        // 2026-10-09; Witchblood, 2026-10-10). Once the compositor did it too,
        // the two cancelled and the record's rect named the texture's LAST rows
        // again: Alien showed the lower part of its picture at the top of the
        // view and unwritten (black) rows under it (a quarter of the view in
        // the owner's screenshot, 2026-10-15). So it is off; the switch stays for an
        // A/B against a compositor that does not mirror.
        //   KL_OVRP_VIEWPORT_TOP=0|1   default 0
        static int top = -1;
        if (top < 0) top = kl_env_on("KL_OVRP_VIEWPORT_TOP", 0);
        if (top && l->desc.texture_size.h > 0)
            for (int eye = 0; eye < 2; eye++) {
                int y = l->desc.texture_size.h - (vp[eye * 4 + 1] + vp[eye * 4 + 3]);
                vp[eye * 4 + 1] = y > 0 ? y : 0;
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
// ovrpLayerFlag_SpaceWarpDataAllocation: "this eye layer also carries motion
// vectors and their depth". AC Nexus sets it (flags=0x300, with the dedicated-
// depth bit beside it) because its OculusSettings asset has SpaceWarp on.
#define KLOVRP_LAYER_FLAG_SPACEWARP 0x100
// KL_OVRP_SPACEWARP_TEX: hand the provider real motion-vector textures when it
// asks. Default on for the targets measured; see
// klovrp_GetLayerTextureSpaceWarp for why refusing is not survivable there.
// Arizona Sunshine 2 (x5245041552210029) has the same provider (eye layer
// flags 0x1300, the seven-argument call at libOculusXRPlugin+0x13c60): its
// display was started and stopped within 40 ms, under GLES and Vulkan alike,
// with only GfxThread_Stop's flush frame ever submitted — a black eye while
// the game and its audio ran on.
static int klovrp_spacewarp_tex(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        // Arizona Sunshine 2 and Ghost Town both request SpaceWarp
        // (flags=0x1300). Ghost Town's native provider has the four
        // stack-address outputs in x3..x6 required by this ABI.
        // Every Unity title under the profile for untested games as well, but
        // only where the call has the seven-argument shape: LEGO Bricktales
        // (flags=0x1300) was refused, its provider stopped creating textures
        // after stage 0, rendered one frame and tore the display down
        // (DestroyDistortionWindow2, DestroyLayer x2) — a black picture with
        // the game running on (2026-10-17). Value 2 = the profile's default,
        // which klovrp_spacewarp_shape_ok checks per call.
        extern int kl_untested_profile(void);
        extern kl_guest_kind kl_driver_kind(void);
        int listed = t && (!strcmp(t, "x5812519008825194") ||
                           !strcmp(t, "x5245041552210029") ||
                           !strcmp(t, "x9531494193591052"));
        int profile = !listed && kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY;
        v = kl_env_on("KL_OVRP_SPACEWARP_TEX", listed || profile);
        if (v && profile && !getenv("KL_OVRP_SPACEWARP_TEX")) v = 2;
    }
    return v;
}
// Whether the two extra out-params of the seven-argument call are this
// thread's stack slots, as OculusXRPlugin passes them. A five-argument caller
// leaves whatever was in x5/x6, which must not be written through; a value
// inside the calling thread's own stack is the shape this ABI needs.
static int klovrp_on_this_stack(const void *p) {
    pthread_t self = pthread_self();
    const char *hi = pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    return p && hi && (const char *)p < hi && (const char *)p >= hi - size;
}
static int klovrp_spacewarp_shape_ok(const void *motion_depth, const void *extra) {
    int v = klovrp_spacewarp_tex();
    if (v != 2) return v;
    int ok = klovrp_on_this_stack(motion_depth) && klovrp_on_this_stack(extra);
    static int said;
    if (!said++)
        fprintf(stderr, "  [ovrp] SpaceWarp textures (profile for untested games): the call %s the "
                        "seven-argument shape\n", ok ? "has" : "does NOT have");
    return ok;
}
// The eye layer's size as the guest ends up allocating it: the offered size times the guest's own
// scale, held to the cap where KL_OVRP_EYE_DESC_CAP is on (below). Exposed for the tests.
void kl_ovrp_eye_desc_size(float texture_scale, int *out_w, int *out_h) {
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    if (!(texture_scale > 0.0f)) texture_scale = 1.0f;
    w = (int)((float)w * texture_scale);
    h = (int)((float)h * texture_scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // KL_OVRP_EYE_DESC_CAP=1: the cap holds for the layer the guest ends up with, not only for the size it
    // is offered. A guest multiplies the offer by a scale of its own: Moss asks for 1.5 in a level and was
    // handed 4608x3697 an eye over a 3072 cap – 18.8 ms of GPU a frame on the headset, 45 frames a second
    // on a 120 Hz display (2026-10-13). After rebelancap's fork, where KL_OVRP_EYE_MAX bounds the desc
    // (be18669). Off by default but for Moss: Beat Saber's 1.2x level scale is wanted, and its caps are
    // sized for it.
    {
        // On for Moss: seen on the headset 2026-10-13 ("es wird wirklich besser mit der einstellung"),
        // 110 frames a second where there were 45. On for Gun Club VR: Unity asks for 1.4 on top, so its
        // 4000 cap became 5600x4492 an eye on the headset (toolchain 557, about 42 frames a second, the
        // panel showing 5600). And on wherever the player chose a "Max resolution per eye" in the start
        // window: that number is meant as the size of the picture, whatever the game multiplies it by.
        // Not cached as a whole: the choice can arrive after the first call in a host run.
        static int env = -2;
        if (env == -2) env = getenv("KL_OVRP_EYE_DESC_CAP") ? kl_env_on("KL_OVRP_EYE_DESC_CAP", 0) : -1;
        const char *t = kl_driver_target_name();
        int on = env >= 0 ? env
               : kl_driver_target_is_moss() || (t && strcmp(t, "x2285803881494677") == 0) || g_eye_max_choice > 0;
        // No display has pushed a size in a host run, so no cap was worked out: the plain default there.
        int cap = g_eye_cap >= 0 ? g_eye_cap : kl_env_int("KL_OVRP_EYE_MAX", 3072);
        // ...times the render scale, so "Resolution" still lowers the picture: held to the plain cap, a
        // guest scale of 1.4 would undo anything down to 71 %.
        if (cap > 0 && g_eye_rs > 0.0f && g_eye_rs < 1.0f) cap = (int)((float)cap * g_eye_rs + 0.5f);
        if (on && cap > 0 && (w > cap || h > cap)) {
            float k = (float)cap / (float)(w > h ? w : h);
            int cw = ((int)((float)w * k + 0.5f) + 1) & ~1, ch = ((int)((float)h * k + 0.5f) + 1) & ~1;
            klovrp_eye_align(&cw, &ch);
            static int said_w, said_h;
            if (cw != said_w || ch != said_h) {
                fprintf(stderr, "  [ovrp] KL_OVRP_EYE_DESC_CAP: the guest's scale %.3f asks for %dx%d an eye, "
                                "held to %dx%d (cap %d)\n", (double)texture_scale, w, h, cw, ch, cap);
                said_w = cw; said_h = ch;
            }
            w = cw; h = ch;
        }
    }

    *out_w = w; *out_h = h;
}
static void klovrp_fill_eye_desc(ovrp_layer_desc_eyefov *d, int layout,
                                 int mip_levels, int sample_count, int format,
                                 int depth_format, int mv_format,
                                 int mv_depth_format,
                                 int layer_flags, float texture_scale) {
    int w = 0, h = 0;
    kl_ovrp_eye_desc_size(texture_scale, &w, &h);

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
    // ...unless the guest asked for Application SpaceWarp and we serve it (see
    // klovrp_GetLayerTextureSpaceWarp): then the motion-vector textures need a
    // size, and it is read from here. A quarter of the eye in each direction —
    // nothing consumes the vectors, so the only thing the number buys is a
    // cheap extra pass for the guest.
    if ((layer_flags & KLOVRP_LAYER_FLAG_SPACEWARP) && klovrp_spacewarp_tex()) {
        d->mv_texture_size = (ovrp_sizei){ (w + 3) / 4, (h + 3) / 4 };
        if (d->mv_texture_size.w < 16) d->mv_texture_size.w = 16;
        if (d->mv_texture_size.h < 16) d->mv_texture_size.h = 16;
    }
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
    // Built whole, handed over in the guest's size (klovrp_guest_desc_size): a
    // plugin before 1.51 has only this form and a desc without the mv fields.
    ovrp_layer_desc_eyefov d;
    klovrp_fill_eye_desc(&d, layout, mip_levels, sample_count, format,
                         depth_format, 10 /* D24_S8, as ...2 itself does */,
                         10, layer_flags, texture_scale);
    memcpy(out, &d, klovrp_guest_desc_size());
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
    // The guest's union is no larger than its eye desc (1.44 copies 0x6c for
    // both in Compositor::ExportLayerDesc), so the same size bounds the clear.
    memset(out, 0, klovrp_guest_desc_size());
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
    // A second eye layer while the first is still LIVE, same geometry: hand it
    // the live layer's GL names too. Vader Immortal (UE 4.19) sets up eye
    // layers 7 and 8 over the live layer 1 without ever destroying it; each
    // fresh name made kl_glfb release the (eye, stage) name before it, and
    // glGenTextures handed those freed numbers straight back for OTHER stages:
    //
    //   [glfb] eye=0 stage=0 replaced with no teardown (GL texture 95 -> 542)
    //   [glfb] eye=0 stage=1 replaced with no teardown (GL texture 96 -> 95)
    //   [glfb] eye=0 stage=0 replaced with no teardown (GL texture 542 -> 97)
    //
    // — GL 95 being layer 1's stage 0, layer 7's stage 1 and layer 8's stage 2
    // in one frame, and the guest's framebuffers built on the earlier meanings.
    // On device a stage then held a frozen picture, composited every third frame
    // with the current pose: a flickering copy of the panel stuck to the view.
    // Sharing the names keeps the one storage kl_glfb already has, which is all
    // its (eye, stage) keying can describe anyway. GL path only (l->tex is
    // empty for a Vulkan guest).
    struct klovrp_layer *share = NULL;
    for (int i = 0; is_eye && !l && i < KLOVRP_MAX_LAYERS && !share; i++) {
        struct klovrp_layer *c = &g_layers[i];
        if (!c->id || !c->is_eye || !c->tex[0][0]) continue;
        if (c->desc.texture_size.w == desc->texture_size.w &&
            c->desc.texture_size.h == desc->texture_size.h &&
            c->desc.layout == desc->layout && c->desc.format == desc->format &&
            g_eye_storage_w == desc->texture_size.w &&
            g_eye_storage_h == desc->texture_size.h)
            share = c;
    }
    for (int i = 0; i < KLOVRP_MAX_LAYERS && !l; i++)
        if (!g_layers[i].id) { l = &g_layers[i]; memset(l, 0, sizeof *l); }
    if (l && share) {
        memcpy(l->tex, share->tex, sizeof l->tex);
        fprintf(stderr, "  [ovrp] SetupLayer: eye layer %d is still live with the same "
                        "%dx%d storage — the new layer shares its GL names\n",
                share->id, desc->texture_size.w, desc->texture_size.h);
    }
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
    // Only the guest's part of the desc is read; an older plugin's has no mv
    // fields, and what lies after it on the guest's stack is not one.
    memset(&l->desc, 0, sizeof l->desc);
    memcpy(&l->desc, desc, klovrp_guest_desc_size());
    l->is_eye = is_eye;
    l->used   = 1;
    *layer_id = l->id;
    // Before any eye texture is handed back, which is when the compositor
    // builds the guest's rate map (kl_glfb_set_eye_doublewide says why).
    if (is_eye) kl_glfb_set_eye_doublewide(desc->layout == KLOVRP_LAYOUT_DOUBLEWIDE);
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
// compositor at the frame boundary — the same layer id appearing again (see the
// note at the test for why frameIndex is no part of it).
static pthread_mutex_t g_enq_mu = PTHREAD_MUTEX_INITIALIZER;
static kl_ovrp_overlay g_enq[KLOVRP_MAX_LAYERS];
static int g_enq_n;
// Every layer id submitted in the frame being gathered, hidden ones included —
// what the boundary test looks at (see klovrp_EnqueueSubmitLayer2).
static int g_enq_seen[KLOVRP_MAX_LAYERS], g_enq_seen_n;
#define KLOVRP_SUBMIT_HIDDEN 0x200
// The layers the enqueue path last published, for klovrp_mirror_enq_gl.
static int g_enq_pub[KLOVRP_MAX_LAYERS], g_enq_pub_n;
static unsigned long long g_enq_pub_tex[KLOVRP_MAX_LAYERS];   // texL as submitted
static pthread_mutex_t g_enq_pub_mu = PTHREAD_MUTEX_INITIALIZER;
static void klovrp_enq_flush(void) {
    // An EMPTY set is published too: a frame in which every panel was hidden
    // has to take the previous frame's panels down, not leave them standing.
    klovrp_enq_publish(g_enq, g_enq_n);
    pthread_mutex_lock(&g_enq_pub_mu);
    g_enq_pub_n = 0;
    for (int i = 0; i < g_enq_n; i++) {
        g_enq_pub_tex[g_enq_pub_n] = g_enq[i].tex;
        g_enq_pub[g_enq_pub_n++] = g_enq[i].layer_id;
    }
    pthread_mutex_unlock(&g_enq_pub_mu);
    g_enq_n = 0;
}
// The enqueue path's panels on a GLES guest. klovrp_record_overlays copies an
// EndFrame4 layer's image into compositor-visible storage (kl_glfb_mirror_layer)
// but the enqueue path never did, so on GLES every enqueued panel reached the
// compositor with no MTLTexture and was dropped ("no placement or no MTLTexture
// — not composited"). Sniper Elite VR shows its narrated intro video on such a
// panel (a 6 m Quad, submitted through ovrp_EnqueueSubmitLayer2 from Unity's
// main thread), so that scene was black. The copy has to run where the guest's
// GL context is current and the panel has just been filled — its render thread,
// at the frame's end — so it is done from EndFrame, not from the enqueue call.
// ...and taking them down again on that same path. ovrp_EndFrame4 ages the
// enqueue set (g_enq_frames_since, klovrp_enq_owns_overlays) but the legacy
// ovrp_EndFrame never did, and the published set is otherwise only replaced by
// the next enqueue. A guest that simply stops enqueueing a panel — Sniper Elite
// VR's loading screen, once the level is in — left it standing in front of the
// player for good. After KLOVRP_ENQ_LIVE_FRAMES end-of-frames with no enqueue,
// publish the empty set once, the same decision EndFrame4's path makes.
static void klovrp_enq_age(void) {
    pthread_mutex_lock(&g_enq_mu);
    unsigned age = __atomic_load_n(&g_enq_frames_since, __ATOMIC_RELAXED);
    if (age >= 1000000u) { pthread_mutex_unlock(&g_enq_mu); return; }
    __atomic_store_n(&g_enq_frames_since, ++age, __ATOMIC_RELAXED);
    if (age != KLOVRP_ENQ_LIVE_FRAMES) { pthread_mutex_unlock(&g_enq_mu); return; }
    // Expiration is a batch boundary too (klovrp_enq_retire): otherwise the
    // unfinished batch comes back with the next panel.
    int retire = klovrp_enq_retire();
    if (retire) { g_enq_n = 0; g_enq_seen_n = 0; }
    pthread_mutex_lock(&g_enq_pub_mu);
    int had = g_enq_pub_n;
    g_enq_pub_n = 0;
    pthread_mutex_unlock(&g_enq_pub_mu);
    if (had) {
        if (!retire) g_enq_n = 0;
        klovrp_enq_publish(g_enq, 0);
    }
    pthread_mutex_unlock(&g_enq_mu);
    if (!had) return;
    static int said;
    if (said++ < 8)
        fprintf(stderr, "  [ovrp] enqueued panels: none submitted for %d frames — "
                        "taken down\n", KLOVRP_ENQ_LIVE_FRAMES);
}

static void klovrp_mirror_enq_gl(void) {
    if (kl_vulkan_guest_active() || !kl_glfb_has_mtl_layer_provider()) return;
    int ids[KLOVRP_MAX_LAYERS], n;
    unsigned long long tex[KLOVRP_MAX_LAYERS];
    pthread_mutex_lock(&g_enq_pub_mu);
    n = g_enq_pub_n;
    memcpy(ids, g_enq_pub, sizeof(int) * (size_t)n);
    memcpy(tex, g_enq_pub_tex, sizeof tex[0] * (size_t)n);
    pthread_mutex_unlock(&g_enq_pub_mu);
    for (int i = 0; i < n; i++) {
        struct klovrp_layer *l = klovrp_layer(ids[i]);
        if (!l || l->is_eye) continue;
        // The texture the guest NAMED in this submit (texL) is the picture: an
        // OVROverlay showing a video or a render texture passes its own GL name
        // there and never draws into the swapchain image we handed it (Sniper
        // Elite VR: storage GL 44, every submit names GL 191). On GLES texL is
        // a GL texture name; the swapchain image is only the fallback.
        uint32_t src = (tex[i] > 0 && tex[i] < 0x100000u) ? (uint32_t)tex[i] : l->tex[0][0];
        if (!src) continue;
        const char *fn = NULL;
        uint32_t gf = klovrp_gl_format(l->desc.format, &fn);
        if (!gf) gf = KL_OVRP_TEXFMT_EYE;
        kl_glfb_mirror_layer(ids[i], 0, src, -1,
                             l->desc.texture_size.w, l->desc.texture_size.h, gf);
    }
}
//
// **...and five more arguments, on the stack.** The 1.34 form this name always
// is continues past x7:
//    Bool overrideTextureRectMatrix, const ovrpTextureRectMatrixf *matrix,
//    Bool overridePerLayerColorScaleAndOffset, const Vector4 *scale, *offset
// (int / pointer / int / pointer / pointer land in the same five 8-byte slots
// under AAPCS64 and under Apple's packed rule, because every pointer re-aligns
// to 8.) The matrix opens with two rects — leftRect, rightRect, each x, y, w, h
// as a FRACTION of the texture — and that is OVROverlay's srcRect: which part
// of the layer's texture each eye shows.
//
// Dropping it shows the whole texture, and a guest that animates a layer by
// moving the rect over a sprite strip then shows every frame at once. Arkham
// Shadow's loading icon is that: a 2048x128 strip of sixteen 128x128 frames on
// a 0.2 m square quad, seen on device as a row of slivers where a bat symbol
// should turn.
//
// The stack words are only what the ABI says they are if the caller really is
// that form, so they are trusted in steps: the flag must be exactly 1, the
// matrix is COPIED with vm_read_overwrite (an error, not a fault, if the word
// was never a pointer), and both rects must be sane fractions.
static int klovrp_read_rect_matrix(const float *m, float out[8]) {
    if ((uintptr_t)m < 0x10000) return 0;
    float v[8];
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)m, sizeof v,
                          (vm_address_t)(uintptr_t)v, &got) != KERN_SUCCESS ||
        got != sizeof v)
        return 0;
    for (int e = 0; e < 2; e++) {
        const float *r = v + 4 * e;
        if (!(r[0] >= -0.001f && r[0] <= 1.0f && r[1] >= -0.001f && r[1] <= 1.0f &&
              r[2] > 0.0f && r[2] <= 1.001f && r[3] > 0.0f && r[3] <= 1.001f))
            return 0;
    }
    memcpy(out, v, sizeof v);
    return 1;
}

static uint64_t klovrp_EnqueueSubmitLayer2(uint32_t flags, void *texL, void *texR,
                                           int layer_id, int frame_index,
                                           const float *pose, const float *scale,
                                           int layer_index,
                                           int override_rect, const float *rect_matrix,
                                           int override_color, const float *color_scale,
                                           const float *color_offset) {
    ovrp_hit("ovrp_EnqueueSubmitLayer2");
    pthread_mutex_lock(&g_enq_mu);
    __atomic_store_n(&g_enq_frames_since, 0, __ATOMIC_RELAXED);
    (void)override_color; (void)color_scale; (void)color_offset;
    { struct klovrp_layer *eq = klovrp_layer(layer_id);
      if (eq && !eq->is_eye && eq->desc.shape == 5 && !(flags & 0x200))
          g_loadvid_frames_since = 0; }
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
    // The frame boundary is the same layer id coming round again, and ONLY
    // that. The frameIndex argument cannot serve: OVROverlay keeps one counter
    // per overlay — 0 for ever on a static one, and on a dynamic one the number
    // of frames since THAT overlay was created — so two panels submitted in the
    // same frame routinely carry different numbers. Reading a changed number as
    // a boundary (what this did) cut every frame of AC Nexus's UI into
    // fragments, each published and replaced within the frame: the published
    // set flipped between { 5 } and { 55..61 } on every flush, and the
    // calibration card, submitted on every frame for fourteen seconds, reached
    // the compositor once.
    //
    // Hidden layers (ovrpLayerSubmitFlag 0x200, OVROverlay.hidden) count for
    // the boundary — they are how a frame of nothing-but-hidden panels is still
    // seen to have begun — but are not handed on: "hidden" means do not
    // composite, and these are the panels the guest has parked.
    int dup = 0;
    for (int i = 0; i < g_enq_seen_n; i++) if (g_enq_seen[i] == layer_id) { dup = 1; break; }
    if (dup) {
        klovrp_enq_flush();
        g_enq_seen_n = 0;
    }
    if (g_enq_seen_n < KLOVRP_MAX_LAYERS) g_enq_seen[g_enq_seen_n++] = layer_id;
    (void)frame_index;
    if (flags & KLOVRP_SUBMIT_HIDDEN) {
        pthread_mutex_unlock(&g_enq_mu);
        return OVRP_SUCCESS;
    }
    struct klovrp_layer *l = klovrp_layer(layer_id);
    // A passthrough layer has no picture to hand on; that it is being filed is
    // the whole of it (klovrp_pt_submits).
    if (l && !l->is_eye && klovrp_shape_is_passthrough(l->desc.shape)) {
        klovrp_pt_enqueued(l->desc.shape);
        if (klovrp_pt_on()) klovrp_pt_resumed(layer_id);
        pthread_mutex_unlock(&g_enq_mu);
        return OVRP_SUCCESS;
    }
    if (l && !l->is_eye && g_enq_n < KLOVRP_MAX_LAYERS) {
        kl_ovrp_overlay *o = &g_enq[g_enq_n++];
        memset(o, 0, sizeof *o);
        o->layer_id = layer_id;
        if (kl_env_on("KL_GHOSTTOWN_OVERLAY_COLOR", 0) &&
            override_color == 1 && color_scale && color_offset &&
            klovrp_color_sane(color_scale) && klovrp_color_sane(color_offset)) {
            o->color_override = 1;
            memcpy(o->color_scale, color_scale, sizeof o->color_scale);
            memcpy(o->color_offset, color_offset, sizeof o->color_offset);
        }
        o->shape    = l->desc.shape;
        o->stage    = 0;   // these layers are set up single-buffered at stage 0
        o->tex_w    = l->desc.texture_size.w;
        o->tex_h    = l->desc.texture_size.h;
        if (pose)  memcpy(o->pose, pose, sizeof o->pose);
        if (scale) { o->size[0] = scale[0]; o->size[1] = scale[1]; }
        // A Cylinder filed here has no union arm to read its shape from: OVROverlay
        // passes it in the scale — arc width, height, radius, in metres. Left at
        // zero, kl_overlay_build refused every such panel, and Spatial Zombies'
        // whole menu (a Cylinder) was never drawn while its pointer clicked on it
        // (2026-10-28).
        //   KL_OVRP_ENQ_CYLINDER=0|1 — default 1 for Spatial Zombies
        //       (x24151641901194680), else 0, where an enqueued cylinder stays
        //       uncomposited as before.
        static int enq_cyl = -1;
        if (enq_cyl < 0) {
            const char *t = kl_driver_target_name();
            enq_cyl = kl_env_on("KL_OVRP_ENQ_CYLINDER", t && !strcmp(t, "x24151641901194680"));
        }
        if (enq_cyl && o->shape == 1 && scale && klovrp_plausible_metres(scale[0]) &&
            klovrp_plausible_metres(scale[1]) && klovrp_plausible_metres(scale[2])) {
            o->cyl[0] = scale[0]; o->cyl[1] = scale[1]; o->cyl[2] = scale[2];
            static int said[128];
            if ((unsigned)layer_id < 128 && !said[layer_id]) {
                said[layer_id] = 1;
                fprintf(stderr, "  [ovrp] EnqueueSubmitLayer2: cylinder layer %d: arc %.3f m, "
                                "height %.3f m, radius %.3f m (%.1f degrees) at (%.2f, %.2f, %.2f)%s\n",
                        layer_id, scale[0], scale[1], scale[2], scale[0] / scale[2] * 57.29578f,
                        pose ? pose[4] : 0.f, pose ? pose[5] : 0.f, pose ? pose[6] : 0.f,
                        o->head_locked ? " head-locked" : "");
            }
        }
        o->flags       = (int)flags;
        o->head_locked = ((int)flags & KLOVRP_SUBMIT_HEAD_LOCKED) != 0;
        // Row 0 is the top of the picture on a Vulkan guest — UNLESS the layer
        // says otherwise, and OVROverlay does: it creates its layers with
        // ovrpLayerFlag_TextureOriginAtBottomLeft (0x8, visible in the
        // CalculateLayerDesc line as flags=0x8), because it fills them from
        // Unity textures, whose row 0 is the bottom on every backend. Taking
        // the backend's word alone drew Arkham Shadow's loading icon upside
        // down, which only became visible once its source rect was honoured.
        o->origin_top_left = kl_vulkan_guest_active() &&
                             !(l->desc.layer_flags & 0x8);
        o->tex = (unsigned long long)(uintptr_t)texL;
        // The source rect, as the per-eye pixel viewport the compositor's
        // overlay pass already honours (kl_overlay_build). Left at zero — "the
        // whole texture" — whenever the guest did not override it.
        float rm[8];
        if (override_rect == 1 && o->tex_w > 0 && o->tex_h > 0 &&
            klovrp_read_rect_matrix(rect_matrix, rm)) {
            for (int e = 0; e < 2; e++) {
                const float *r = rm + 4 * e;
                o->viewport[e][0] = (int)(r[0] * (float)o->tex_w + 0.5f);
                o->viewport[e][1] = (int)(r[1] * (float)o->tex_h + 0.5f);
                o->viewport[e][2] = (int)(r[2] * (float)o->tex_w + 0.5f);
                o->viewport[e][3] = (int)(r[3] * (float)o->tex_h + 0.5f);
            }
            static int said[128];
            if ((unsigned)layer_id < 128 && !said[layer_id]) {
                said[layer_id] = 1;
                fprintf(stderr, "  [ovrp] EnqueueSubmitLayer2: layer %d carries a "
                                "source rect — left %.4f,%.4f %.4fx%.4f of its "
                                "%dx%d texture (%dx%d px); composited as that "
                                "part, not the whole\n",
                        layer_id, (double)rm[0], (double)rm[1], (double)rm[2],
                        (double)rm[3], o->tex_w, o->tex_h,
                        o->viewport[0][2], o->viewport[0][3]);
            }
        }
    }
    pthread_mutex_unlock(&g_enq_mu);
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
    pthread_mutex_lock(&g_enq_mu);
    struct klovrp_layer *l = klovrp_layer(layer_id);
    // A panel's compositor-side rows go back for another id's use (the eye
    // layer's stay, see below): only said here, freed on the mirroring thread
    // (kl_glfb_forget_layer).
    if (l && !l->is_eye) kl_glfb_forget_layer(layer_id);
    if (l && !l->is_eye && klovrp_enq_retire()) {
        // Removing the allocation alone leaves a published overlay pointing
        // at retained Vulkan storage. Retire it from both the visible snapshot
        // and the pending enqueue batch, so a later flush cannot resurrect it.
        pthread_mutex_lock(&g_frames.mu);
        for (int i = 0; i < g_overlays.n; ) {
            if (g_overlays.v[i].layer_id != layer_id) { ++i; continue; }
            memmove(&g_overlays.v[i], &g_overlays.v[i + 1],
                    (size_t)(g_overlays.n - i - 1) * sizeof g_overlays.v[0]);
            --g_overlays.n;
        }
        pthread_mutex_unlock(&g_frames.mu);
        for (int i = 0; i < g_enq_n; ) {
            if (g_enq[i].layer_id != layer_id) { ++i; continue; }
            memmove(&g_enq[i], &g_enq[i + 1],
                    (size_t)(g_enq_n - i - 1) * sizeof g_enq[0]);
            --g_enq_n;
        }
        pthread_mutex_lock(&g_enq_pub_mu);
        for (int i = 0; i < g_enq_pub_n; ) {
            if (g_enq_pub[i] != layer_id) { ++i; continue; }
            --g_enq_pub_n;
            memmove(&g_enq_pub[i], &g_enq_pub[i + 1],
                    (size_t)(g_enq_pub_n - i) * sizeof g_enq_pub[0]);
            memmove(&g_enq_pub_tex[i], &g_enq_pub_tex[i + 1],
                    (size_t)(g_enq_pub_n - i) * sizeof g_enq_pub_tex[0]);
        }
        pthread_mutex_unlock(&g_enq_pub_mu);
        fprintf(stderr, "  [ovrp] retired overlay %d from visible and queued layers\n", layer_id);
    }
    // The teardown mirror. The GL names are NOT deleted here, and the slot keeps
    // them: this arrives on the provider's gfx-thread stop, the eye textures are
    // still attached to framebuffers the capture and the compositor hold, and
    // deleting a name the compositor is sampling is a worse failure than holding
    // it across a restart that is about to ask for the same layer again.
    // klovrp_SetupLayer above is the other half — it hands the same textures back.
    if (l) { l->prev_id = l->id; l->id = 0; }
    for (int i = 0; klovrp_enq_retire() && i < g_enq_seen_n; ) {
        if (g_enq_seen[i] != layer_id) { ++i; continue; }
        --g_enq_seen_n;
        memmove(&g_enq_seen[i], &g_enq_seen[i + 1],
                (size_t)(g_enq_seen_n - i) * sizeof g_enq_seen[0]);
    }
    pthread_mutex_unlock(&g_enq_mu);
    kl_loadvid_recount();
    return OVRP_SUCCESS;
}

static int klovrp_layer_id_arg(uintptr_t raw);
static uint64_t klovrp_EnqueueDestroyLayer(uintptr_t layer_arg);

// Host regression gate: isolated process, no graphics device or guest content.
int kl_ovrp_layer_lifecycle_selftest(void) {
    ovrp_layer_desc_eyefov desc = {0};
    desc.texture_size = (ovrp_sizei){16, 16};
    int a = 0, b = 0;
    if (klovrp_SetupLayer(NULL, &desc, &a)) return 0;
    float pose[7] = {0, 0, 0, 1, 0, 0, -2}, size[3] = {1, 1, 1};
#define ENQUEUE(id, flags) klovrp_EnqueueSubmitLayer2(flags, NULL, NULL, id, 0, pose, size, 0, 0, NULL, 0, NULL, NULL)
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "layer lifecycle failed: %s\n", #x); return 0; } } while (0)
    ENQUEUE(a, 0); ENQUEUE(a, 0);
    REQUIRE(g_enq_n == 1 && kl_ovrp_overlay_count() == 1);
    // EndFrame4's no-submit interval must clear BOTH pending and published,
    // even if the visible set has already been cleared by another producer.
    for (int i = 0; i < KLOVRP_ENQ_LIVE_FRAMES; ++i) klovrp_EndFrame4(i, NULL, 0, NULL);
    REQUIRE(g_enq_n == 0 && g_enq_seen_n == 0 && g_enq_pub_n == 0);
    REQUIRE(kl_ovrp_overlay_count() == 0);
    ENQUEUE(a, 0);
    REQUIRE(g_enq_n == 1 && g_enq_pub_n == 0);
    for (int i = 0; i < KLOVRP_ENQ_LIVE_FRAMES; ++i) klovrp_enq_age();
    REQUIRE(g_enq_n == 0 && g_enq_seen_n == 0);
    ENQUEUE(a, 0); ENQUEUE(a, 0);
    klovrp_DestroyLayer(a);
    REQUIRE(g_enq_n == 0 && g_enq_seen_n == 0 && g_enq_pub_n == 0);
    REQUIRE(kl_ovrp_overlay_count() == 0);
    REQUIRE(!klovrp_SetupLayer(NULL, &desc, &b) && b != a);
    ENQUEUE(b, 0); ENQUEUE(b, 0);
    kl_ovrp_overlay out;
    REQUIRE(kl_ovrp_overlay_count() == 1 && kl_ovrp_overlay_get(0, &out) && out.layer_id == b);
    ENQUEUE(b, KLOVRP_SUBMIT_HIDDEN); ENQUEUE(b, KLOVRP_SUBMIT_HIDDEN);
    REQUIRE(kl_ovrp_overlay_count() == 0);
    klovrp_DestroyLayer(b);
    // The pointer form: the id lives at the address the guest passes.
    int handle = 0;
    REQUIRE(!klovrp_SetupLayer(NULL, &desc, &handle) && handle != 0);
    ENQUEUE(handle, 0); ENQUEUE(handle, 0);
    REQUIRE(kl_ovrp_overlay_count() == 1);
    klovrp_EnqueueDestroyLayer((uintptr_t)&handle);
    REQUIRE(g_enq_n == 0 && g_enq_seen_n == 0 && g_enq_pub_n == 0);
    REQUIRE(kl_ovrp_overlay_count() == 0 && !klovrp_layer(handle));
    ENQUEUE(handle, 0); ENQUEUE(handle, 0);
    REQUIRE(kl_ovrp_overlay_count() == 0);
#undef REQUIRE
#undef ENQUEUE
    return 1;
}

// The argument is not one shape. AC Nexus passes the layer id as an int.
// Ghost Town's plugin treats x0 as a pointer (it null-checks the full
// register and hashes that pointer): it is the same int* EnqueueSetupLayer2
// wrote the id through. Reading the low 32 bits of that pointer looks up a
// missing id, so the quad stays published after the game has destroyed it.
static int klovrp_layer_id_arg(uintptr_t raw) {
    if (raw < 0x10000) return (int)raw;
    int id = 0;
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)raw, sizeof id,
                          (vm_address_t)&id, &got) == KERN_SUCCESS &&
        got == sizeof id && klovrp_layer(id))
        return id;
    if (klovrp_layer((int)raw)) return (int)raw;
    return 0;
}

// ovrp_EnqueueDestroyLayer(layerId) - the Enqueue-ABI sibling of DestroyLayer,
// the teardown half of ovrp_EnqueueSetupLayer2. AC Nexus's menu drives its
// OVROverlay panels this way: EnqueueSetupLayer2 to make one, EnqueueDestroyLayer
// to free it. Without this handler the destroy was an unimplemented no-op, so
// every panel the game freed kept its slot forever and the 32-slot table filled
// in seconds - SetupLayer then failed for the rest of the run and the render
// loop stalled. Frees the slot the same careful way DestroyLayer does (the
// textures are kept for reuse, only the id is cleared). A retired id is not a
// live layer, so a later submit of it cannot be published again.
static uint64_t klovrp_EnqueueDestroyLayer(uintptr_t layer_arg) {
    ovrp_hit("ovrp_EnqueueDestroyLayer");
    int layer_id = klovrp_enq_retire() ? klovrp_layer_id_arg(layer_arg) : (int)layer_arg;
    return klovrp_DestroyLayer(layer_id);
}

    // ovrp_GetLayerTextureStageCount(layerId, int* out) — real 0x16e160,
    // -1001 on a NULL second argument.
// ovrp_GetLayerRecommendedResolution(int layerId, ovrpSizei* out) — signature
// read from redmatter2's libOVRPlugin (x0=layerId, x1=out). The recommended
// resolution IS the size we already put in the layer's desc; eye layers fall
// back to the eye texture size. Red Matter 2 aborts on the missing entry point.
static uint64_t klovrp_GetLayerRecommendedResolution(int layer_id, ovrp_sizei *out) {
    ovrp_hit("ovrp_GetLayerRecommendedResolution");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    struct klovrp_layer *l = klovrp_layer(layer_id);
    if (l && l->desc.texture_size.w > 0) {
        *out = l->desc.texture_size;
    } else {
        int w = 0, h = 0; kl_ovrp_eye_texture_size(&w, &h);
        out->w = w; out->h = h;
    }
    // An Unreal guest with dynamic resolution sets its eye layer up at its
    // LARGEST pixel density and asks here, every frame, how much of it to draw
    // into. Answering with the layer's own size says "all of it, always":
    // Red Matter 2 on the headset set up 6144x3944, drew 4915x3944 an eye
    // (its ceiling of 1.6) in every frame and ran at half the display's rate.
    // The size that is recommended is the one CalculateEyeLayerDesc gives at
    // density 1 — the eye texture size. For this title first; the others that
    // ask have been seen with the layer's size and keep it until looked at.
    // Red Matter (x2180753588712484, the first game, the same Unreal 4.27 with
    // the same OVRPlugin) does the same: on the headset it set up 7776x4932,
    // grew its viewport to 6133x4930 an eye within a minute and ran at 50 Hz
    // (2026-10-19). KL_OVRP_RECO_EYE=0|1.
    if (l && l->is_eye) {
        static int base = -1;
        if (base < 0) {
            const char *t = kl_driver_target_name();
            base = kl_env_on("KL_OVRP_RECO_EYE", t && (strcmp(t, "redmatter2") == 0 ||
                                                       strcmp(t, "x2180753588712484") == 0));
        }
        int w = 0, h = 0; kl_ovrp_eye_texture_size(&w, &h);
        if (base && w > 0 && h > 0 && (w < out->w || h < out->h)) {
            static int said;
            if (!said++) fprintf(stderr, "  [ovrp] ovrp_GetLayerRecommendedResolution: the eye layer is "
                                         "%dx%d, recommended is %dx%d (KL_OVRP_RECO_EYE)\n",
                                 out->w, out->h, w, h);
            out->w = w; out->h = h;
        }
    }
    // The eye layer's follows the GPU where that is asked for (klovrp_adapt_note).
    if (klovrp_adapt_on() && (!l || l->is_eye)) {
        static int said;
        if (!said++) fprintf(stderr, "  [ovrp] ovrp_GetLayerRecommendedResolution is asked for the eye "
                                     "layer (%dx%d) — KL_OVRP_ADAPTIVE_RES answers it\n", out->w, out->h);
        float k = g_adapt_scale;
        out->w = ((int)((float)out->w * k + 0.5f) + 1) & ~1;
        out->h = ((int)((float)out->h * k + 0.5f) + 1) & ~1;
    }
    return OVRP_SUCCESS;
}

// Three entry points a 1.117 guest (Virtual Virtual Reality, Meta XR SDK on
// Unity's OpenXR plugin) calls once the plugin version unlocks them. Shapes read
// from that APK's own libOVRPlugin.so:
//   ovrp_AllowVisibilityMask(int)            logs, stores (arg != 0) in a global,
//                                            returns success
//   ovrp_SetExternalLayerDynresEnabled(int)  forwards to the session, ovrpResult
//   ovrp_GetEyeLayerRecommendedResolution(ovrpSizei *out)
//                                            x0 IS the out parameter (8 bytes,
//                                            w then h), written on success
// The two switches change nothing here — the eye layer's size is ours to answer
// and the visibility mask is the OpenXR runtime's — so they record the request
// and succeed. The resolution is the eye texture size every other size query
// in this file answers, so the guest cannot be told two different eyes.
static uint64_t klovrp_AllowVisibilityMask(int enabled) {
    ovrp_hit("ovrp_AllowVisibilityMask");
    (void)enabled;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_SetExternalLayerDynresEnabled(int enabled) {
    ovrp_hit("ovrp_SetExternalLayerDynresEnabled");
    (void)enabled;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetEyeLayerRecommendedResolution(ovrp_sizei *out) {
    ovrp_hit("ovrp_GetEyeLayerRecommendedResolution");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    out->w = w; out->h = h;
    return OVRP_SUCCESS;
}

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
    int gl_array = l->is_eye && l->desc.layout == KLOVRP_LAYOUT_ARRAY &&
                   !kl_vulkan_guest_active() && kl_ovrp_gl_array();
    if (l->is_eye && l->desc.layout == KLOVRP_LAYOUT_ARRAY &&
        kl_vulkan_guest_active() && kl_ovrp_multiview()) {
        /* served below */
    } else if (gl_array) {
        /* served below, as a GL array texture */
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
        int kind = glfmt == 0x881A ? KLVK_LAYER_FP16 // GL_RGBA16F
                 : srgb ? KLVK_LAYER_SRGB : KLVK_LAYER_UNORM;
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
                                  stage, eye, (unsigned)w, (unsigned)h, kind, layers);
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

    // The Array layout on GL (kl_ovrp_gl_array): ONE two-layer array texture per
    // stage, and both eyes are handed the same name — the guest addresses the eye
    // by layer (glFramebufferTextureMultiviewOVR). Plain GL storage: nothing can
    // bind an array to the compositor's texture, so it is copied at EndFrame4.
    if (gl_array) {
        uint32_t *slot = &l->tex[stage][0];
        if (!*slot) {
            static void (*gl_GenTextures)(int32_t, uint32_t *);
            static void (*gl_BindTexture)(uint32_t, uint32_t);
            static void (*gl_TexStorage3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);
            static void (*gl_TexParameteri)(uint32_t, uint32_t, int32_t);
            static void (*gl_GetIntegerv)(uint32_t, int32_t *);
            if (!gl_GenTextures) {
                gl_GenTextures   = kl_egl_sym("glGenTextures");
                gl_BindTexture   = kl_egl_sym("glBindTexture");
                gl_TexStorage3D  = kl_egl_sym("glTexStorage3D");
                gl_TexParameteri = kl_egl_sym("glTexParameteri");
                gl_GetIntegerv   = kl_egl_sym("glGetIntegerv");
            }
            uint32_t name = 0;
            if (gl_GenTextures) gl_GenTextures(1, &name);
            if (!name || !gl_BindTexture || !gl_TexStorage3D) {
                fprintf(stderr, "  [ovrp] GetLayerTexture2(layer %d stage %d): no GL array "
                                "texture could be made — no GL context here\n", layer_id, stage);
                return OVRP_FAIL_NOT_INITIALIZED;
            }
            int w = l->desc.texture_size.w, h = l->desc.texture_size.h;
            const char *fname = NULL;
            uint32_t glfmt = klovrp_gl_format(l->desc.format, &fname);
            if (!glfmt) glfmt = KL_OVRP_TEXFMT_EYE;
            // The guest's own array binding is put back: this runs in the middle
            // of its texture set-up, on its context.
            int32_t prev = 0;
            if (gl_GetIntegerv) gl_GetIntegerv(0x8C1D /* TEXTURE_BINDING_2D_ARRAY */, &prev);
            gl_BindTexture(0x8C1A /* TEXTURE_2D_ARRAY */, name);
            gl_TexStorage3D(0x8C1A, 1, glfmt, w, h, 2);
            if (gl_TexParameteri) {
                gl_TexParameteri(0x8C1A, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
                gl_TexParameteri(0x8C1A, 0x2800 /* MAG_FILTER */, 0x2601);
                gl_TexParameteri(0x8C1A, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
                gl_TexParameteri(0x8C1A, 0x2803 /* WRAP_T */, 0x812F);
            }
            gl_BindTexture(0x8C1A, (uint32_t)prev);
            *slot = name;
            l->tex[stage][1] = name;
            g_eye_storage_w = w; g_eye_storage_h = h;
            fprintf(stderr, "  [ovrp] GetLayerTexture2: eye layer %d stage %d = GL array texture %u "
                            "(%dx%d x2 layers, %s / GL %#x) — both eyes share it, copied to the "
                            "compositor at EndFrame\n",
                    layer_id, stage, name, w, h, fname ? fname : "?", glfmt);
        }
        *color = *slot;
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
        // No context on this thread (the guest asked from one that holds
        // none): make the storage under a borrowed share-group context instead
        // of refusing the layer. See kl_glfb_borrow_context.
        int borrowed = 0;
        if (!name && gl_GenTextures && kl_glfb_borrow_context()) {
            borrowed = 1;
            gl_GenTextures(1, &name);
        }
        if (!name) {
            if (borrowed) kl_glfb_return_context();
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
            // Shared, not bound: binding the same GL name for eye 1 would move
            // its storage to slice 1 (see kl_glfb_share_eye_mtl_texture).
            if (l->desc.layout == KLOVRP_LAYOUT_DOUBLEWIDE) {
                l->tex[stage][1] = name;
                kl_glfb_note_eye_texture(1, stage, name);
                if (mtl_backed)
                    kl_glfb_share_eye_mtl_texture(1, stage, 0);
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
        if (borrowed) kl_glfb_return_context();
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
        if (kl_glfb_has_mtl_provider())
            kl_glfb_share_eye_mtl_texture(eye, stage, canon);
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
// ...which is true of the provider that note was written from, and false of AC
// Nexus's (Oculus XR Plugin on OVRPlugin 1.60, SpaceWarp enabled in its
// settings). There OculusDisplayProvider::CreateMotionVectorTexture treats any
// failure as fatal for the STAGE: "Failed to get spaceWarp layer textures" ->
// "CreateEyeTextureStages failed." -> "Failed to create eye textures.", and
// Unity answers a failed first frame by stopping the display provider for good
// (GfxThread_Stop, "Destroying distortion window"). Everything after that runs
// with no XR display: one frame submitted in the whole run, XRSettings reporting
// a 0x0 eye texture, and only OVROverlay quads ever reaching the compositor.
//
// So for that provider the textures are handed out. Its call has SEVEN
// arguments, read off its own call site (libOculusXRPlugin+0x13c28):
//
//   ovrp_GetLayerTextureSpaceWarp(layerId, stage, eye,
//                                 &motionVector, &a, &motionVectorDepth, &b)
//
// and it uses the first and third out-params as the colour and depth native
// textures of a Unity render texture of desc.mv_texture_size (two array layers
// under the Array layout); the other two it never reads, so they are zeroed.
// The five-argument shape above is what every other caller gets, untouched —
// its x5/x6 are not pointers and must not be written through.
static uint64_t klovrp_GetLayerTextureSpaceWarp(int layer_id, int stage, int eye,
                                                uint64_t *motion, uint64_t *depth,
                                                uint64_t *motion_depth, uint64_t *extra) {
    ovrp_hit("ovrp_GetLayerTextureSpaceWarp");
    if (!motion || !depth) return OVRP_FAIL_INVALID_PARAM;
    struct klovrp_layer *l = klovrp_layer(layer_id);
    if (klovrp_spacewarp_tex() && kl_vulkan_guest_active() && l && l->is_eye &&
        (l->desc.layer_flags & KLOVRP_LAYER_FLAG_SPACEWARP) &&
        motion_depth && extra && l->desc.mv_texture_size.w > 0 &&
        klovrp_spacewarp_shape_ok(motion_depth, extra)) {
        int layers = (l->desc.layout == KLOVRP_LAYOUT_ARRAY) ? 2 : 1;
        unsigned w = (unsigned)l->desc.mv_texture_size.w;
        unsigned h = (unsigned)l->desc.mv_texture_size.h;
        unsigned long long c = kl_vulkan_spacewarp_image(0, stage, w, h, layers);
        unsigned long long z = kl_vulkan_spacewarp_image(1, stage, w, h, layers);
        if (c && z) {
            *motion = c;
            *depth = 0;
            *motion_depth = z;
            *extra = 0;
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [ovrp] GetLayerTextureSpaceWarp: layer %d stage %d eye %d "
                                "-> motion vectors %#llx, depth %#llx (%ux%u, %d layer(s)); "
                                "rendered into by the guest, consumed by nothing\n",
                        layer_id, stage, eye, c, z, w, h, layers);
            }
            return OVRP_SUCCESS;
        }
    }
    (void)stage; (void)eye;
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
//
// ...except that on Vulkan the name of this entry is literal: what the caller
// wants is a POINTER to the VkImage, not the VkImage. OVROverlay hands the value
// straight to Texture2D.CreateExternalTexture, and Unity's Vulkan backend reads
// a native texture pointer as VkImage* - the same shape kl_vulkan_mtl_for_handle
// already found on the way back (texL is a pointer whose first word is the
// image). Handed the VkImage itself, Unity dereferences it: the first word of a
// MoltenVK image object is its C++ vtable, so every layer yields the SAME
// "image", pointing into MoltenVK's read-only data. That is AC Nexus's
// deterministic wild handle in vkCreateImageView (0x...d99d0 on every layer,
// moving only with the ASLR slide), and why its overlay textures never held a
// picture. Tied to the one target it was traced on; KL_OVRP_TEXPTR_INDIRECT
// overrides in either direction.
static int klovrp_texptr_indirect(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        // Ghost Town (x9531494193591052) does the same read. GetLayerTexturePtr
        // handed it the VkImage, the next vkCreateImageView was on one wild
        // handle ending in the MoltenVK vtable (…d99d0) for every fade layer,
        // and the 1×1 quad it stretches across the view stayed the blank it
        // was cleared to — a white sheet, with the fade's real pixel never
        // stored. The indirect word is what OVROverlay passes to
        // CreateExternalTexture.
        v = kl_env_on("KL_OVRP_TEXPTR_INDIRECT",
                      t && (!strcmp(t, "x5812519008825194") ||
                            !strcmp(t, "x9531494193591052")));
    }
    return v;
}
// One stable word per distinct image, because the guest may read the pointer
// again long after this call returns. Images are recycled by value, so the
// table stops growing once the guest's set of layers has been seen.
static uint64_t *klovrp_texptr_slot(uint64_t img) {
    static uint64_t slots[4096];
    static int n;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    uint64_t *s = NULL;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) if (slots[i] == img) { s = &slots[i]; break; }
    if (!s && n < (int)(sizeof slots / sizeof slots[0])) { s = &slots[n++]; *s = img; }
    pthread_mutex_unlock(&lk);
    return s;
}
static uint64_t klovrp_GetLayerTexturePtr(int layer_id, int stage, int eye,
                                          uint64_t *texture_ptr) {
    ovrp_hit("ovrp_GetLayerTexturePtr");
    uint64_t color = 0, depth = 0;
    uint64_t r = klovrp_GetLayerTexture2(layer_id, stage, eye, &color, &depth);
    uint64_t out = color;
    if (color && kl_vulkan_guest_active() && klovrp_texptr_indirect()) {
        uint64_t *slot = klovrp_texptr_slot(color);
        if (slot) out = (uint64_t)(uintptr_t)slot;
    }
    if (texture_ptr) *texture_ptr = out;
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
//
// ...except for an Unreal title's Equirect layer. Medal of Honor: Above and
// Beyond plays its 360 gallery films that way (ANative360VideoPlayer): it sets
// the layer up in the menu, asks for the layer's Surface, and once a film is
// picked hands it, with the film's path, to its Java GameActivity.playVideo,
// which runs ExoPlayer into it. So the layer gets a stand-in
// android/view/Surface; playVideo (kl_jni_ue4.c) names the file to
// kl_loadingvideo_set_source, and the compositor's Equirect skybox plays it.
// KL_OVRP_LAYER_SURFACE=0/1 overrides; default on for UE4 guests only, so AC
// Nexus (Unity) keeps the refusal it was tuned against.
static uint64_t klovrp_GetLayerAndroidSurfaceObject(int layer_id, void **surface) {
    ovrp_hit("ovrp_GetLayerAndroidSurfaceObject");
    if (surface) *surface = NULL;
    static int on = -1;
    if (on < 0) {
        extern kl_guest_kind kl_driver_kind(void);
        on = kl_env_on("KL_OVRP_LAYER_SURFACE", kl_driver_kind() == KL_GUEST_UE4);
    }
    int equirect = 0;
    for (int i = 0; on && i < KLOVRP_MAX_LAYERS; i++)
        if (g_layers[i].id == layer_id && layer_id && !g_layers[i].is_eye && g_layers[i].desc.shape == 5) {
            equirect = 1; break;
        }
    if (!equirect || !surface) return OVRP_FAIL_UNSUPPORTED;
    extern void *kl_jni_new_object(const char *class_name);
    *surface = kl_jni_new_object("android/view/Surface");
    static int said;
    if (!said) { said = 1;
        fprintf(stderr, "  [ovrp] GetLayerAndroidSurfaceObject(layer %d, Equirect) -> a stand-in "
                        "Surface: the video the guest plays into it is drawn as the loading skybox\n",
                layer_id); }
    return *surface ? OVRP_SUCCESS : OVRP_FAIL_UNSUPPORTED;
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

// Eye tracking: the guest asks whether the runtime can hand it a per-eye gaze
// vector. Vision Pro deliberately never exposes gaze to a guest process, so the
// honest and safe answer is "not supported" — the guest then skips its
// eye-tracked foveation / gaze paths entirely instead of calling
// ovrp_GetEyeGazesState and dereferencing a struct we never fill. Takes an
// ovrpBool* out and returns ovrpResult (0 = success), so *out=0 is FALSE.
static uint64_t klovrp_GetEyeTrackingSupported(int *out) {
    ovrp_hit("ovrp_GetEyeTrackingSupported");
    if (out) *out = 0;
    return 0;
}
static uint64_t klovrp_GetEyeTrackingEnabled(int *out) {
    ovrp_hit("ovrp_GetEyeTrackingEnabled");
    if (out) *out = 0;
    return 0;
}

// Body and face tracking: the same question as eye tracking, about the Quest's
// own body estimate and face cameras. Neither exists here — the headset gives a
// guest its head and its hands, nothing of the torso or the face — so "not
// supported" and "not enabled" are the truthful answers, and with them the
// guest has no reason to start either. (Deadpool VR reads the body state all the
// same; that and the start/stop calls say UNSUPPORTED, see the shared list
// further down.) Marvel's Deadpool VR asks
// whether body tracking is supported once it has loaded, seven minutes in, and
// with no entry point it stopped there (2026-10-09). Each takes an ovrpBool* out
// and returns ovrpResult, exactly like the eye-tracking pair above.
#define KLOVRP_TRACKING_ABSENT(fn)                                              \
    static uint64_t klovrp_##fn(int *out) {                                     \
        ovrp_hit("ovrp_" #fn);                                                  \
        if (out) *out = 0;                                                      \
        return 0;                                                               \
    }
KLOVRP_TRACKING_ABSENT(GetBodyTrackingSupported)
KLOVRP_TRACKING_ABSENT(GetBodyTrackingEnabled)
KLOVRP_TRACKING_ABSENT(GetFullBodyTrackingEnabled)
KLOVRP_TRACKING_ABSENT(GetFaceTrackingSupported)
KLOVRP_TRACKING_ABSENT(GetFaceTrackingEnabled)

// Passthrough capabilities: a bitmask behind an out-param (ovrpPassthroughCapabilityFlags*)
// and an ovrpResult return. Witchblood asks right after ovrp_InitializeInsightPassthrough
// was refused, on its first composited frame, and with no entry point it stopped there
// (2026-10-10). No passthrough for it (klovrp_pt_on), so the mask is
// empty — not even the Passthrough bit — and the call itself succeeds, so a guest
// that falls back on an UNSUPPORTED result has no second question to ask.
// Where passthrough is offered: Passthrough (1<<0) and Color (1<<1) — the room
// in a mixed space is in colour.
static uint64_t klovrp_GetPassthroughCapabilityFlags(int *out) {
    ovrp_hit("ovrp_GetPassthroughCapabilityFlags");
    if (out) *out = klovrp_pt_on() ? 0x3 : 0;
    return OVRP_SUCCESS;
}

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
static void *g_unity_interfaces;
static void klovrp_UnityPluginLoad(void *unity_interfaces) {
    g_unity_interfaces = unity_interfaces;
    fprintf(stderr, "  [ovrp] UnityPluginLoad(%p) — recorded; no graphics device "
                    "bound yet\n", unity_interfaces);
}
// `ovrpResult f(IUnityInterfaces **out)` (-1001 on NULL): the pointer Unity
// handed UnityPluginLoad, given back. Meta's OpenXR-backend plugins ask for it
// (DreamSpace, 2026-10-16).
static uint64_t klovrp_GetUnityInterfaces(void **out) {
    ovrp_hit("ovrp_GetUnityInterfaces");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_unity_interfaces;
    return OVRP_SUCCESS;
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

// ovrp_GetHandTrackingEnabled(ovrpBool* out): is the SYSTEM's hand tracking
// switched on? Yes for a guest that gets hands (kl_ovrp_set_hand_joints), even
// while a controller is held — on a Quest this is the system setting, not the
// current input. Otherwise a successful "false": Maestro (UE4, OculusInput) asks
// once at start and has a controller path for that answer; unanswered, it
// stopped on the trampoline.
static int32_t klovrp_GetHandTrackingEnabled(int *out) {
    ovrp_hit("ovrp_GetHandTrackingEnabled");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = klovrp_hands_title();
    return OVRP_SUCCESS;
}

static int32_t klovrp_skeleton_fill(const char *name, int type, void *out, int max_bones);

// ovrp_GetSkeleton2 / ovrp_GetSkeleton3 (ovrpSkeletonType 0..5, out struct): the
// same Bone and BoneCapsule records as ovrp_GetSkeleton with room for more bones
// — 70 in Skeleton2Internal (0xc44 bytes), 84 in Skeleton3Internal (0xe3c), the
// 19 capsules after them. Layouts read from Angry Birds' own metadata
// (OVRPlugin 1.101: Bones_0 at +12, BoneCapsules_0 at +12+36*N). Only the two
// OVR hand types are answered; Body and the OpenXR hand skeleton stay refused.
static int32_t klovrp_GetSkeleton2(int skeleton_type, void *out) {
    if (skeleton_type < 0 || skeleton_type > 5 || !out) return OVRP_FAIL_INVALID_PARAM;
    return klovrp_skeleton_fill("ovrp_GetSkeleton2", skeleton_type, out, 70);
}
static int32_t klovrp_GetSkeleton3(int skeleton_type, void *out) {
    if (skeleton_type < 0 || skeleton_type > 5 || !out) return OVRP_FAIL_INVALID_PARAM;
    return klovrp_skeleton_fill("ovrp_GetSkeleton3", skeleton_type, out, 84);
}

// ovrp_GetControllerIsInHand(ovrpStep, ovrpNode, ovrpBool *): is the controller
// held? For a guest with hands (kl_ovrp_set_hand_joints): no while the hands are
// the input, yes while a Sense controller is. Other guests keep the answer they
// had — success with the Bool left alone (the shared permissive path).
static int32_t klovrp_GetControllerIsInHand(int step, int node, int *out) {
    ovrp_hit("ovrp_GetControllerIsInHand");
    (void)step; (void)node;
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    if (klovrp_hands_title()) *out = !klovrp_hands_mode();
    return OVRP_SUCCESS;
}

// KL_OVRP_FAKE_HANDS=1 (Mac host runs, diagnostic): both hands in front of the
// head, palms down, the fingers closing and opening every four seconds — what
// the frontend publishes on the headset, made up. Published from the guest's
// own reads, so there must be no frontend writing hands at the same time.
static void klovrp_fake_hands_tick(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVRP_FAKE_HANDS", 0) && klovrp_hands_title();
    if (!on) return;
    static double last;
    double now = klovrp_mono_now();
    if (now - last < 0.008) return;
    last = now;
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    float hq[4] = { h.qx, h.qy, h.qz, h.qw };
    (void)hq;
    float fx, fy, fz, rx, ry, rz;
    klovrp_qrot(&h, 0, 0, -1, &fx, &fy, &fz);
    klovrp_qrot(&h, 1, 0, 0, &rx, &ry, &rz);
    float curl = 45.0f * (1.0f - cosf((float)(now * 2 * 3.14159265 / 4.0)));
    for (int hand = 0; hand < 2; hand++) {
        float side = hand ? 0.15f : -0.15f;
        float pos[3] = { h.px + fx * 0.35f + rx * side, h.py - 0.25f + fy * 0.35f,
                         h.pz + fz * 0.35f + rz * side };
        pos[1] += klovrp_origin_y();        // published in the frontend's world
        float fwd[3] = { fx, fy, fz };
        kl_hand_xf j[KLH_JOINTS];
        kl_hands_synth(hand, pos, fwd, curl, j);
        float xf[KLH_JOINTS * 7];
        for (int i = 0; i < KLH_JOINTS; i++) {
            memcpy(xf + 7 * i, j[i].p, 3 * sizeof(float));
            memcpy(xf + 7 * i + 3, j[i].q, 4 * sizeof(float));
        }
        kl_ovrp_set_hand_joints(hand, 1, xf, (1u << KLH_JOINTS) - 1);
    }
}

// Index pinches as the buttons a Quest reports with hands: A on the right, X on
// the left (OVRInput's hands map), so a guest's "press A" works with a pinch.
static uint32_t klovrp_hands_buttons(void) {
    klovrp_fake_hands_tick();
    pthread_mutex_lock(&g_hj_lock);
    uint32_t l = g_hj_ovr[0].tracked ? g_hj_ovr[0].pinch_bits : 0;
    uint32_t r = g_hj_ovr[1].tracked ? g_hj_ovr[1].pinch_bits : 0;
    pthread_mutex_unlock(&g_hj_lock);
    return ((r & 2u) ? 0x1u : 0) | ((l & 2u) ? 0x100u : 0);
}

// ovrpHandState (OVRP_1_44_0's HandStateInternal), 0x200 bytes — the size the
// 1.45 plugin memcpy's out (CompositorVRAPI::GetHandState):
//   +0x000 Status            +0x1a0 Pinches          +0x1d8 HandConfidence
//   +0x004 RootPose (q, p)   +0x1a4 PinchStrength[5] +0x1dc FingerConfidences[5]
//   +0x020 BoneRotations[24] +0x1b8 PointerPose      +0x1f0 RequestedTimeStamp
//                            +0x1d4 HandScale        +0x1f8 SampleTimeStamp
// Status: HandTracked 1, InputStateValid 2, DominantHand 0x80 (the right hand).
// Confidence: Low 0, High 0x3f800000.
static void klovrp_hand_state_fill(int hand, void *out) {
    klovrp_fake_hands_tick();
    kl_ovr_hand o;
    kl_hand_xf raw[KLH_JOINTS];
    double t;
    pthread_mutex_lock(&g_hj_lock);
    o = g_hj_ovr[hand];
    memcpy(raw, g_hj_raw[hand], sizeof raw);
    t = g_hj_time[hand];
    pthread_mutex_unlock(&g_hj_lock);
    uint8_t *b = out;
    memset(b, 0, 0x200);
    if (o.root.q[3] == 0 && o.root.q[0] == 0 && o.root.q[1] == 0 && o.root.q[2] == 0) {
        // Never seen: identities rather than zero quaternions.
        o.root.q[3] = 1;
        for (int i = 0; i < KLO_BONES; i++) { memset(o.rot[i], 0, 12); o.rot[i][3] = 1; }
        o.scale = 1;
    }
    int32_t status = o.tracked ? 3 : 0;
    if (hand == 1) status |= 0x80;
    memcpy(b + 0x00, &status, 4);
    memcpy(b + 0x04, o.root.q, 16);
    memcpy(b + 0x14, o.root.p, 12);
    memcpy(b + 0x20, o.rot, sizeof o.rot);
    uint32_t pinches = o.tracked ? o.pinch_bits : 0;
    memcpy(b + 0x1a0, &pinches, 4);
    if (o.tracked) memcpy(b + 0x1a4, o.pinch, 20);
    // The pointer ray: from an estimated shoulder through the hand, smoothed by
    // the publisher (kl_ovrp_set_hand_joints).
    pthread_mutex_lock(&g_hj_lock);
    kl_hand_xf ptr = g_hj_ptr[hand];
    pthread_mutex_unlock(&g_hj_lock);
    (void)raw;
    memcpy(b + 0x1b8, ptr.q, 16);
    memcpy(b + 0x1c8, ptr.p, 12);
    memcpy(b + 0x1d4, &o.scale, 4);
    uint32_t hi = 0x3f800000u;
    if (o.tracked && o.high_confidence) memcpy(b + 0x1d8, &hi, 4);
    for (int f = 0; f < 5; f++)
        if (o.tracked && o.finger_high[f]) memcpy(b + 0x1dc + 4 * f, &hi, 4);
    double now = kl_ovrp_predicted_display_time();
    memcpy(b + 0x1f0, &now, 8);
    memcpy(b + 0x1f8, t > 0 ? &t : &now, 8);
    g_hj_reads[hand]++;
    if (!o.tracked) g_hj_reads_lost[hand]++;
    {
        static double at;
        double now = klovrp_mono_now();
        if (at == 0) at = now;
        if (now - at >= 5.0) {
            at = now;
            fprintf(stderr, "  [hands] guest, last 5 s: reads L %u (untracked %u) R %u (untracked %u); "
                            "held over a gap L %u R %u frames; jumps held back L %u R %u; "
                            "no hand frame L %u R %u\n",
                    g_hj_reads[0], g_hj_reads_lost[0], g_hj_reads[1], g_hj_reads_lost[1],
                    g_hj_held[0], g_hj_held[1], g_hj_jumps[0], g_hj_jumps[1],
                    g_hj_failed[0], g_hj_failed[1]);
            g_hj_reads[0] = g_hj_reads[1] = g_hj_reads_lost[0] = g_hj_reads_lost[1] = 0;
            g_hj_held[0] = g_hj_held[1] = g_hj_failed[0] = g_hj_failed[1] = 0;
            g_hj_jumps[0] = g_hj_jumps[1] = 0;
        }
    }
    static int said[2];
    if (o.tracked && !said[hand]++)
        fprintf(stderr, "  [hands] guest reads the %s hand: tracked, root %.2f %.2f %.2f, scale %.2f\n",
                hand ? "right" : "left", o.root.p[0], o.root.p[1], o.root.p[2], o.scale);
}

// ovrp_GetHandState(ovrpStep, ovrpHand, ovrpHandState *) — OVRP_1_44_0.
static int32_t klovrp_GetHandState(int step, int hand, void *out) {
    ovrp_hit("ovrp_GetHandState");
    (void)step;
    if (!klovrp_hands_title()) return OVRP_FAIL_UNSUPPORTED;
    if ((unsigned)hand > 1 || !out) return OVRP_FAIL_INVALID_PARAM;
    klovrp_hand_state_fill(hand, out);
    return OVRP_SUCCESS;
}

// ovrp_GetHandState2(ovrpStep, int frameIndex, ovrpHand, ovrpHandState *) —
// the same struct, one more argument (OVRP_1_55_0... per OVRPlugin.cs).
static int32_t klovrp_GetHandState2(int step, int frame, int hand, void *out) {
    ovrp_hit("ovrp_GetHandState2");
    (void)step; (void)frame;
    if (!klovrp_hands_title()) return OVRP_FAIL_UNSUPPORTED;
    if ((unsigned)hand > 1 || !out) return OVRP_FAIL_INVALID_PARAM;
    klovrp_hand_state_fill(hand, out);
    return OVRP_SUCCESS;
}

// ovrp_GetSkeleton(ovrpSkeletonType, ovrpSkeleton *) — OVRP_1_44_0, 0x5cc bytes
// (the 1.45 plugin zeroes 0x5c0 past the 12-byte head):
//   +0x000 Type  +0x004 NumBones  +0x008 NumBoneCapsules
//   +0x00c Bones[24], 0x24 each: Id i32, ParentBoneIndex i16 (+2 pad), Pose (q, p)
//   +0x36c Capsules[19], 0x20 each: BoneIndex i16 (+2), StartPoint, EndPoint, Radius
// Types: HandLeft 0, HandRight 1. The pose is kl_hands.c's own generic hand.
static int32_t klovrp_skeleton_fill(const char *name, int type, void *out, int max_bones) {
    ovrp_hit(name);
    if (!klovrp_hands_title()) return OVRP_FAIL_UNSUPPORTED;
    if ((unsigned)type > 1) {
        static int said;
        if (!said++) fprintf(stderr, "  [hands] %s asked for skeleton type %d — only the OVR hand "
                                     "types (0, 1) are answered\n", name, type);
        return OVRP_FAIL_UNSUPPORTED;
    }
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    kl_ovr_skel sk;
    kl_hands_bind(type, &sk);
    uint8_t *b = out;
    size_t caps = 0x0c + 0x24 * (size_t)max_bones;
    memset(b, 0, caps + 0x20 * KLO_CAPSULES);
    int32_t head[3] = { type, KLO_BONES, KLO_CAPSULES };
    memcpy(b, head, 12);
    for (int i = 0; i < KLO_BONES; i++) {
        uint8_t *e = b + 0x0c + 0x24 * i;
        int32_t id = i;
        memcpy(e, &id, 4);
        memcpy(e + 4, &sk.bone[i].parent, 2);
        memcpy(e + 8, sk.bone[i].pose.q, 16);
        memcpy(e + 24, sk.bone[i].pose.p, 12);
    }
    for (int i = 0; i < KLO_CAPSULES; i++) {
        uint8_t *e = b + caps + 0x20 * i;
        memcpy(e, &sk.cap[i].bone, 2);
        memcpy(e + 4, sk.cap[i].a, 12);
        memcpy(e + 16, sk.cap[i].b, 12);
        memcpy(e + 28, &sk.cap[i].radius, 4);
    }
    fprintf(stderr, "  [hands] guest asked for the %s hand skeleton (%s): 24 bones, 19 capsules\n",
            type ? "right" : "left", name);
    return OVRP_SUCCESS;
}

static int32_t klovrp_GetSkeleton(int type, void *out) {
    if ((unsigned)type > 1 && klovrp_hands_title()) return OVRP_FAIL_INVALID_PARAM;
    return klovrp_skeleton_fill("ovrp_GetSkeleton", type, out, 24);
}

// ovrp_GetMesh: the hand's skinned mesh. Not made here (no mesh of our own yet);
// refused like before, said once so a guest that needs it shows in the log.
static int32_t klovrp_GetMesh(int type, void *out) {
    ovrp_hit("ovrp_GetMesh");
    (void)out;
    static int said;
    if (klovrp_hands_title() && !said++)
        fprintf(stderr, "  [hands] guest asked for the hand mesh (type %d) — refused, none here\n", type);
    return OVRP_FAIL_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// The rest of Meta's OpenXR-backend startup (OVRPlugin 1.203.0, DreamSpace,
// measured in a KL_PERMISSIVE scouting run on 2026-10-09). Shapes read out of
// that APK's libOVRPlugin.so; the out-parameter ones are here, the pure setters
// and the refused subsystems in the shared lists below.

// `ovrpResult f(int *handle)`: -1000 on NULL, else an int handle. No telemetry
// pipe here; a nonzero handle every Telemetry* setter accepts and drops.
static uint64_t klovrp_TelemetryCreateMetadataHandle(int *out) {
    ovrp_hit("ovrp_TelemetryCreateMetadataHandle");
    if (!out) return (uint64_t)(int64_t)-1000;
    *out = 1;
    return OVRP_SUCCESS;
}

// `ovrpResult f(const char **out)` — the name kl_openxr puts in
// XrInstanceProperties.runtimeName, so the two cannot disagree. Copied once into
// a buffer of our own: the guest may keep the pointer for the rest of the run.
static char g_runtime_name[128];   // XR_MAX_RUNTIME_NAME_SIZE, as kl_openxr's copy
static void klovrp_runtime_name_once(void) {
    snprintf(g_runtime_name, sizeof g_runtime_name, "%s",
             kl_env_str("KL_XR_RUNTIME_NAME", "Oculus"));
}
static uint64_t klovrp_GetRuntimeName(const char **out) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    ovrp_hit("ovrp_GetRuntimeName");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    pthread_once(&once, klovrp_runtime_name_once);
    *out = g_runtime_name;
    return OVRP_SUCCESS;
}

// `ovrpResult f(XrSpace *out)`: the space Unity named through
// ovrp_UnityOpenXR_OnAppSpaceChange[2]. Before it has, a failure with the
// out-parameter untouched.
static uint64_t klovrp_GetAppSpace(uint64_t *out) {
    ovrp_hit("ovrp_GetAppSpace");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    if (!g_uox_app_space) return OVRP_FAIL_OPERATION;
    *out = g_uox_app_space;
    return OVRP_SUCCESS;
}

// (hand, step, frameIndex, ovrpBool *out): no inferred hand pose — hand tracking
// is refused as a group (klovrp_hand_unsupported).
static uint64_t klovrp_GetHandPoseSourceInferred(int hand, int step, int frame, int *out) {
    (void)hand; (void)step; (void)frame;
    ovrp_hit("ovrp_GetHandPoseSourceInferred");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// `ovrpResult f(XrInstance *, XrSession *)` and `ovrpResult f(PFN *)`. Meta's
// MR Utility Kit asks for these as soon as it loads, to make its own OpenXR
// calls — its scene loading never goes through ovrp_* at all. The answers are
// kl_openxr's own, the runtime Unity already talks to, so the kit finds out
// from that runtime what is and is not served.
static uint64_t klovrp_GetNativeOpenXRHandles(uint64_t *instance, uint64_t *session) {
    ovrp_hit("ovrp_GetNativeOpenXRHandles");
    if (!instance || !session) return OVRP_FAIL_INVALID_PARAM;
    if (!kl_openxr_native_handles(instance, session)) return OVRP_FAIL_NOT_INITIALIZED;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetOpenXRInstanceProcAddrFunc(void **out) {
    ovrp_hit("ovrp_GetOpenXRInstanceProcAddrFunc");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = kl_openxr_get_instance_proc_addr();
    return OVRP_SUCCESS;
}

// OpenXR events, forwarded. On Meta's OpenXR backend the real plugin hooks
// xrPollEvent and hands every event to the handlers registered here
// (`ovrpResult f(void (*)(void *event, void *context), void *context)`; the
// event is an XrEventDataBuffer). Meta's MR Utility Kit does not poll OpenXR
// itself — it registers its OnOpenXrEvent and learns from it that a space
// discovery has results — so a registration that is accepted and never called
// leaves it waiting for a room until it times out ("genuinely wedged",
// DreamSpace, 2026-10-16). kl_openxr calls kl_ovrp_dispatch_openxr_event for
// each event its xrPollEvent hands out.
typedef void (*klovrp_xr_event_fn)(void *event, void *context);
#define KLOVRP_XR_HANDLERS 8
static struct { klovrp_xr_event_fn fn; void *ctx; } g_xr_handlers[KLOVRP_XR_HANDLERS];
static pthread_mutex_t g_xr_handlers_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t klovrp_RegisterOpenXREventHandler(klovrp_xr_event_fn fn, void *ctx) {
    ovrp_hit("ovrp_RegisterOpenXREventHandler");
    if (!fn) return OVRP_FAIL_INVALID_PARAM;
    pthread_mutex_lock(&g_xr_handlers_mu);
    int i = 0;
    for (; i < KLOVRP_XR_HANDLERS; i++) if (!g_xr_handlers[i].fn) break;
    if (i < KLOVRP_XR_HANDLERS) { g_xr_handlers[i].fn = fn; g_xr_handlers[i].ctx = ctx; }
    pthread_mutex_unlock(&g_xr_handlers_mu);
    fprintf(stderr, "  [ovrp] OpenXR event handler %p (context %p) %s\n", (void *)fn, ctx,
            i < KLOVRP_XR_HANDLERS ? "registered" : "NOT registered — table full");
    return OVRP_SUCCESS;
}
static uint64_t klovrp_UnregisterOpenXREventHandler(klovrp_xr_event_fn fn) {
    ovrp_hit("ovrp_UnregisterOpenXREventHandler");
    pthread_mutex_lock(&g_xr_handlers_mu);
    for (int i = 0; i < KLOVRP_XR_HANDLERS; i++)
        if (g_xr_handlers[i].fn == fn) { g_xr_handlers[i].fn = NULL; g_xr_handlers[i].ctx = NULL; }
    pthread_mutex_unlock(&g_xr_handlers_mu);
    return OVRP_SUCCESS;
}
void kl_ovrp_dispatch_openxr_event(void *event) {
    klovrp_xr_event_fn fn[KLOVRP_XR_HANDLERS]; void *ctx[KLOVRP_XR_HANDLERS]; int n = 0;
    pthread_mutex_lock(&g_xr_handlers_mu);
    for (int i = 0; i < KLOVRP_XR_HANDLERS; i++)
        if (g_xr_handlers[i].fn) { fn[n] = g_xr_handlers[i].fn; ctx[n] = g_xr_handlers[i].ctx; n++; }
    pthread_mutex_unlock(&g_xr_handlers_mu);
    for (int i = 0; i < n; i++) fn[i](event, ctx[i]);   // outside the lock: a handler may re-enter
}

static const char g_ovrp_handle[] = "klepton-ovrplugin";

// Assembly entry thunks that capture the x8 sret pointer before any call can
// clobber it (kl_ovrp_sret.S); the _impl bodies are above.
void klovrp_GetNodePoseState_entry(void);
void klovrp_GetControllerState2_entry(void);
void klovrp_GetControllerHapticsDesc_entry(void);
void klovrp_GetControllerState_entry(void);
void klovrp_GetTrackingCalibratedOrigin_entry(void);

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

// ===========================================================================
// Entry points taken over from rebelancap's fork (avp-play issue 16, round 4,
// from his be18669 and what followed it). Each of these was unknown to this
// file: a guest that resolved one got the stub that aborts by name when it is
// called. They are answers to questions no guest listed here had asked, so
// nothing a shipped game does changes. Where this file already answered a
// name — through a handler of its own or one of the lists below — its answer
// stays, and his is not taken.
//
// The few pieces of state they keep among themselves come first.
#ifndef OVRP_CTRL_TOUCH
#define OVRP_CTRL_TOUCH (OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH)
#endif
#ifndef OVRP_FAILURE
#define OVRP_FAILURE (-1000)
#endif
typedef struct { float up, down, left, right; } klovrp_fovf;
static int   g_app_cpu_priority = 1; // the real plugin's default is "yes"
static int   g_client_color_space;   // ovrpColorSpace_Unknown until a client says
static float g_head_pose_mod[7] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
static struct {
    int initialized;
    int activation_mode;
    int audio_sample_rate;
    int frame_w, frame_h;
    int frame_image_flipped;
    int frame_inverse_alpha;
} g_media;

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

// ovrpPosef out: identity orientation, and a pure vertical offset.
static uint64_t klovrp_pose_out_y(float *out, float y) {
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;   // quat x,y,z,w
    out[4] = 0.0f; out[5] = y;    out[6] = 0.0f;                  // position
    return OVRP_SUCCESS;
}

// The tracking supported/enabled sweep UE4.27 (deadpool) makes beside the
// capability "no"s: ovrpResult + ovrpBool* out, like the 2-forms above. 6DoF
// head tracking exists here (the compositor supplies poses), so each answers
// the Quest's own: position and orientation supported and on, IPD tracking on.
// On the deadpool branch these sat in g_ovrp_bool_yes, which returned 1 and
// left the out-slot holding the guest's own stack — the bug class driver
// already fixed for ovrp_Media_GetInitialized.
#define KLOVRP_OUT_TRUE(fn)                                   \
    static uint64_t klovrp_##fn(int *out) {                   \
        ovrp_hit("ovrp_" #fn);                                \
        if (!out) return (uint64_t)(int64_t)OVRP_FAIL_INVALID_PARAM; \
        *out = 1;                                             \
        return OVRP_SUCCESS;                                  \
    }
KLOVRP_OUT_TRUE(GetTrackingOrientationSupported2)
KLOVRP_OUT_TRUE(GetTrackingPositionEnabled2)
KLOVRP_OUT_TRUE(GetTrackingOrientationEnabled2)
KLOVRP_OUT_TRUE(GetTrackingIPDEnabled2)
#undef KLOVRP_OUT_TRUE

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
// `ovrp_GetHandTrackingEnabled` is the one that was FATAL. The Last Clockwinder's
// boot #2 reached the frame pump, composited 1693 frames, and then died on
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
KLOVRP_CAPABILITY_OFF(GetTiledMultiResDynamic)
KLOVRP_CAPABILITY_OFF(GetSystemHmd3DofModeEnabled)
KLOVRP_CAPABILITY_OFF(GetGPUUtilSupported)
#undef KLOVRP_CAPABILITY_OFF

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

static klovrp_fovf klovrp_GetNodeFrustum(int node) {
    ovrp_hit("ovrp_GetNodeFrustum");
    // The tangents as the guest is told them everywhere else (klovrp_eye_tan),
    // not the raw measurement: his fork reads g_eye_tan here.
    const float *t = klovrp_eye_tan(node == 1 ? 1 : 0);
    return (klovrp_fovf){t[2], t[3], t[0], t[1]};
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

// The 1.0 form, which Onward's OVRPlugin.cs still calls: the 28-byte ovrpPosef
// returned BY VALUE, so the destination arrives in x8 (kl_ovrp_sret.S). The
// real plugin (0x1595a8 in Onward's copy) is a wrapper round the 2-form that
// stores the identity pose when that one fails; ours never does.
void klovrp_GetTrackingCalibratedOrigin_impl(float *out) {
    ovrp_hit("ovrp_GetTrackingCalibratedOrigin");
    klovrp_pose_out_y(out, 0.0f);
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

KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTextureDesc,
                       "ovrp_GetEnvironmentDepthTextureDesc")

KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTexture,
                       "ovrp_GetEnvironmentDepthTexture")

KLOVRP_ENVDEPTH_ABSENT(klovrp_GetEnvironmentDepthTextureStageCount,
                       "ovrp_GetEnvironmentDepthTextureStageCount")

#define KLOVRP_PASSTHROUGH_REFUSE(fn, name)                     \
    static uint64_t fn(void) { return klovrp_refuse(name); }

KLOVRP_PASSTHROUGH_REFUSE(klovrp_SetInsightPassthroughKeyboardHandsIntensity,
                          "ovrp_SetInsightPassthroughKeyboardHandsIntensity")

// Colour look-up tables for passthrough: a mixed-reality title tints or darkens
// the room through one (DreamSpace's themes — Batcave and the rest — each build
// one when chosen, and threw "Passthrough Color LUTs are not supported" and
// loaded nothing; 2026-10-16). The room here is visionOS's own passthrough, which
// takes no colour table, so a table is ACCEPTED and not applied: the theme loads
// and the room keeps its own colours. Offered (capability and entry points) only
// where KL_OVRP_PT_LUT says so — default DreamSpace — and refused as before
// everywhere else. Shapes from the real 1.203.0 plugin:
//   Create(int channels, int resolution, {uint size; void *buf} data, uint64 *lut)
//   Update(uint64 lut, {size, buf} data)        Destroy(uint64 lut)
// (the 16-byte data struct travels in two registers, so the out-param is x4).
static int klovrp_pt_lut_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = klovrp_pt_on() && kl_env_on("KL_OVRP_PT_LUT", t && !strcmp(t, "x9449316705178989"));
        if (on) fprintf(stderr, "  [ovrp] passthrough: colour LUTs accepted, not applied (KL_OVRP_PT_LUT)\n");
    }
    return on;
}
static uint64_t g_pt_next_lut = 0x6b6c4c00;
static uint64_t klovrp_CreatePassthroughColorLut(int channels, int resolution, uint64_t size,
                                                 const void *buf, uint64_t *lut) {
    if (!klovrp_pt_lut_on()) return klovrp_refuse("ovrp_CreatePassthroughColorLut");
    ovrp_hit("ovrp_CreatePassthroughColorLut");
    (void)size; (void)buf;
    if (!lut) return OVRP_FAIL_INVALID_PARAM;
    if (resolution <= 0 || resolution > KLOVRP_PT_LUT_MAX_RES) return OVRP_FAIL_INVALID_PARAM;
    *lut = ++g_pt_next_lut;
    static int said;
    if (said++ < 4)
        fprintf(stderr, "  [ovrp] passthrough: colour LUT %d channels, %d^3 -> %#llx (not applied)\n",
                channels, resolution, (unsigned long long)*lut);
    return OVRP_SUCCESS;
}
static uint64_t klovrp_UpdatePassthroughColorLut(uint64_t lut, uint64_t size, const void *buf) {
    if (!klovrp_pt_lut_on()) return klovrp_refuse("ovrp_UpdatePassthroughColorLut");
    ovrp_hit("ovrp_UpdatePassthroughColorLut");
    (void)size; (void)buf;
    return lut ? OVRP_SUCCESS : OVRP_FAIL_INVALID_PARAM;
}
static uint64_t klovrp_DestroyPassthroughColorLut(uint64_t lut) {
    if (!klovrp_pt_lut_on()) return klovrp_refuse("ovrp_DestroyPassthroughColorLut");
    ovrp_hit("ovrp_DestroyPassthroughColorLut");
    return lut ? OVRP_SUCCESS : OVRP_FAIL_INVALID_PARAM;
}

static uint64_t klovrp_Media_Shutdown(void) {
    ovrp_hit("ovrp_Media_Shutdown");
    g_media.initialized = 0;
    return OVRP_SUCCESS;
}

// The three predicates that keep MRC switched off, and the debug-camera one
// beside them. NO is what is true: nothing here activates a capture, nothing
// casts to a remote client, and there is no debug camera to substitute.
static uint64_t klovrp_Media_IsMrcEnabled(int *out) {
    ovrp_hit("ovrp_Media_IsMrcEnabled");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_IsMrcActivated(int *out) {
    ovrp_hit("ovrp_Media_IsMrcActivated");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_UseMrcDebugCamera(int *out) {
    ovrp_hit("ovrp_Media_UseMrcDebugCamera");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_IsCastingToRemoteClient(int *out) {
    ovrp_hit("ovrp_Media_IsCastingToRemoteClient");
    if (out) *out = 0;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_GetMrcActivationMode(int *out) {
    ovrp_hit("ovrp_Media_GetMrcActivationMode");
    if (out) *out = g_media.activation_mode;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_GetMrcAudioSampleRate(int *out) {
    ovrp_hit("ovrp_Media_GetMrcAudioSampleRate");
    if (out) *out = g_media.audio_sample_rate;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_SetMrcFrameSize(int w, int h) {
    ovrp_hit("ovrp_Media_SetMrcFrameSize");
    g_media.frame_w = w; g_media.frame_h = h;
    return OVRP_SUCCESS;
}

// Two out-pointers, x0 and x1 (+0x1d7b40 moves both before the tail call), and
// the pair starts at 0x0 because nothing here sizes an MRC frame until the guest
// says how big it wants one.
static uint64_t klovrp_Media_GetMrcFrameSize(int *w, int *h) {
    ovrp_hit("ovrp_Media_GetMrcFrameSize");
    if (w) *w = g_media.frame_w;
    if (h) *h = g_media.frame_h;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_GetMrcFrameImageFlipped(int *out) {
    ovrp_hit("ovrp_Media_GetMrcFrameImageFlipped");
    if (out) *out = g_media.frame_image_flipped;
    return OVRP_SUCCESS;
}

static uint64_t klovrp_Media_GetMrcFrameInverseAlpha(int *out) {
    ovrp_hit("ovrp_Media_GetMrcFrameInverseAlpha");
    if (out) *out = g_media.frame_inverse_alpha;
    return OVRP_SUCCESS;
}

// The rest of the BODY-tracking surface, and the reason it is a family rather
// than the one name that aborted.
//
// deadpool's UOculusBodyTrackingComponent does not stop at GetBodyState4. On
// the gameplay -> main-menu transition (the component being re-created against
// a fresh world) it calls ovrp_RequestBodyTrackingFidelity, which was in no
// list — so it took the unimplemented-entry-point abort at the top of this file
// and the app died with SIGABRT right after printing its OVRPlugin work list.
// That is the signature recorded as "the session ended on SIGABRT, and it is
// not yet explained" in deadpool/STATUS.md's first device round; it was this,
// and the fatal line was simply above the report in the log.
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

static uint64_t klovrp_GetAppCPUPriority2(int *out) {
    ovrp_hit("ovrp_GetAppCPUPriority2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = g_app_cpu_priority;
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

// Touch, or the hands while they are the input (kl_ovrp_set_hand_joints).
static uint32_t klovrp_ctrl_present(void) {
    return klovrp_hands_mode() ? OVRP_CTRL_HANDS : OVRP_CTRL_TOUCH | klovrp_hands_beside_bits();
}

static uint64_t klovrp_GetConnectedControllers2(int *out) {
    ovrp_hit("ovrp_GetConnectedControllers2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (int)klovrp_ctrl_present();
    return OVRP_SUCCESS;
}

static uint64_t klovrp_GetActiveController2(int *out) {
    ovrp_hit("ovrp_GetActiveController2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = (int)klovrp_ctrl_present();
    return OVRP_SUCCESS;
}

// The un-suffixed forms, OVRP_1_1_0's: `ovrpController ovrp_GetConnectedControllers(void)`
// and `ovrpController ovrp_GetActiveController(void)` RETURN the mask instead of
// writing it through a pointer. Unity's OVRInput reads them every frame once the
// Meta half of the SDK is up on the OpenXR backend (Banners & Bastions,
// x8387152738076399, told its own 1.201.0; 2026-10-17). Unanswered they ended
// the game, and under KL_OVRP_UNANSWERED=unsupported the -1004 came back AS the
// mask — no Touch, a gamepad and a remote that are not there — and OVRInput
// read neither the PSVR2 Sense controllers nor the hands. The same answer as
// the 2-forms above: the pair of Touch controllers this runtime presents.
static uint64_t klovrp_GetConnectedControllers(void) {
    ovrp_hit("ovrp_GetConnectedControllers");
    return (uint64_t)klovrp_ctrl_present();
}
static uint64_t klovrp_GetActiveController(void) {
    ovrp_hit("ovrp_GetActiveController");
    return (uint64_t)klovrp_ctrl_present();
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
    // His fork answers yes for the eye layer alone. This compositor also draws
    // quads (0), cylinders (1) and the equirect loading video (5), and says so.
    *out = (shape == KLOVRP_SHAPE_EYEFOV || shape == 0 || shape == 1 || shape == 5) ? 1 : 0;
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
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    out[0] = out[1] = out[2] = 0.0f;
    if (klovrp_boundary(out)) {
        static int said;
        if (!said++) fprintf(stderr, "  [ovrp] ovrp_GetBoundaryDimensions2(type 0x%x) asked (KL_OVRP_BOUNDARY)\n", type);
    }
    return OVRP_SUCCESS;
}

// Under KL_OVRP_BOUNDARY only (kl_ovrp_gated_out): the v1 configured flag and
// the geometry, a rectangle of the play area on the floor round the origin.
// Geometry2/3 share one shape — (type, ovrpVector3f *points, int *count) — and
// are called twice, first with no buffer for the count. 2 returns ovrpBool, 3
// ovrpResult.
static uint64_t klovrp_GetBoundaryConfigured(void) {
    ovrp_hit("ovrp_GetBoundaryConfigured");
    return (uint64_t)klovrp_boundary(NULL);
}
static int klovrp_boundary_rect(int type, float *pts, int *count, const char *name) {
    float d[3];
    if (!count || !klovrp_boundary(d)) return 0;
    static int said;
    if (!said++) fprintf(stderr, "  [ovrp] %s(type 0x%x) asked (KL_OVRP_BOUNDARY)\n", name, type);
    float hx = d[0] * 0.5f, hz = d[2] * 0.5f, y = -klovrp_origin_y();
    const float r[4][3] = { { -hx, y, -hz }, { hx, y, -hz }, { hx, y, hz }, { -hx, y, hz } };
    if (pts) {
        int n = *count < 4 ? *count : 4;
        for (int i = 0; i < n; i++) { pts[3*i] = r[i][0]; pts[3*i+1] = r[i][1]; pts[3*i+2] = r[i][2]; }
    }
    *count = 4;
    return 1;
}
static uint64_t klovrp_GetBoundaryGeometry2(int type, float *pts, int *count) {
    ovrp_hit("ovrp_GetBoundaryGeometry2");
    return (uint64_t)klovrp_boundary_rect(type, pts, count, "ovrp_GetBoundaryGeometry2");
}
static uint64_t klovrp_GetBoundaryGeometry3(int type, float *pts, int *count) {
    ovrp_hit("ovrp_GetBoundaryGeometry3");
    if (!count) return OVRP_FAIL_INVALID_PARAM;
    return klovrp_boundary_rect(type, pts, count, "ovrp_GetBoundaryGeometry3")
           ? OVRP_SUCCESS : (uint64_t)(int64_t)OVRP_FAIL_OPERATION;
}

static uint64_t klovrp_GetBoundaryVisible2(int *out) {
    ovrp_hit("ovrp_GetBoundaryVisible2");
    if (!out) return OVRP_FAIL_INVALID_PARAM;
    *out = 0;
    return OVRP_SUCCESS;
}

// ===========================================================================

static const struct { const char *name; void *fn; } g_ovrp_impl[] = {
    // Taken over from rebelancap's fork — see the block above the table.
    {"ovrp_CreatePassthroughColorLut", (void *)klovrp_CreatePassthroughColorLut},
    {"ovrp_DestroyEnvironmentDepth", (void *)klovrp_DestroyEnvironmentDepth},
    {"ovrp_DestroyPassthroughColorLut", (void *)klovrp_DestroyPassthroughColorLut},
    {"ovrp_GetASWAdaptiveMode", (void *)klovrp_GetASWAdaptiveMode},
    {"ovrp_GetASWDepthScale", (void *)klovrp_GetASWDepthScale},
    {"ovrp_GetASWVelocityScale", (void *)klovrp_GetASWVelocityScale},
    {"ovrp_GetActiveController2", (void *)klovrp_GetActiveController2},
    {"ovrp_GetActiveController", (void *)klovrp_GetActiveController},
    {"ovrp_GetAppCPUPriority2", (void *)klovrp_GetAppCPUPriority2},
    {"ovrp_GetAppFramerate2", (void *)klovrp_GetAppFramerate2},
    {"ovrp_GetAppHasSystemOverlayPresent", (void *)klovrp_GetAppHasSystemOverlayPresent},
    {"ovrp_GetAudioInDeviceId2", (void *)klovrp_GetAudioInDeviceId2},
    {"ovrp_GetAudioOutDeviceId2", (void *)klovrp_GetAudioOutDeviceId2},
    {"ovrp_GetBodySkeleton", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodySkeleton2", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState2", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyState3", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyTrackingCalibrationStatus", (void *)klovrp_body_unsupported},
    {"ovrp_GetBodyTrackingFidelityStatus", (void *)klovrp_body_unsupported},
    {"ovrp_GetBoundaryDimensions2", (void *)klovrp_GetBoundaryDimensions2},
    {"ovrp_GetBoundaryConfigured", (void *)klovrp_GetBoundaryConfigured},
    {"ovrp_GetBoundaryGeometry2", (void *)klovrp_GetBoundaryGeometry2},
    {"ovrp_GetBoundaryGeometry3", (void *)klovrp_GetBoundaryGeometry3},
    {"ovrp_GetBoundaryVisible2", (void *)klovrp_GetBoundaryVisible2},
    {"ovrp_GetConnectedControllers2", (void *)klovrp_GetConnectedControllers2},
    {"ovrp_GetConnectedControllers", (void *)klovrp_GetConnectedControllers},
    {"ovrp_GetCurrentTrackingTransformPose", (void *)klovrp_GetCurrentTrackingTransformPose},
    {"ovrp_GetEnvironmentDepthHandRemovalSupported", (void *)klovrp_GetEnvironmentDepthHandRemovalSupported},
    {"ovrp_GetEnvironmentDepthSupported", (void *)klovrp_GetEnvironmentDepthSupported},
    {"ovrp_GetEnvironmentDepthTexture", (void *)klovrp_GetEnvironmentDepthTexture},
    {"ovrp_GetEnvironmentDepthTextureDesc", (void *)klovrp_GetEnvironmentDepthTextureDesc},
    {"ovrp_GetEnvironmentDepthTextureStageCount", (void *)klovrp_GetEnvironmentDepthTextureStageCount},
    {"ovrp_GetEyePixelsPerTanAngleAtCenter2", (void *)klovrp_GetEyePixelsPerTanAngleAtCenter2},
    {"ovrp_GetGPUUtilSupported", (void *)klovrp_GetGPUUtilSupported},
    {"ovrp_GetHeadPoseModifier", (void *)klovrp_GetHeadPoseModifier},
    {"ovrp_GetHmdColorDesc", (void *)klovrp_GetHmdColorDesc},
    {"ovrp_GetHmdToEyeOffset2", (void *)klovrp_GetHmdToEyeOffset2},
    {"ovrp_GetNodeFrustum", (void *)klovrp_GetNodeFrustum},
    {"ovrp_GetNodePoseStateRaw", (void *)klovrp_GetNodePoseStateRaw},
    {"ovrp_GetSystemHmd3DofModeEnabled", (void *)klovrp_GetSystemHmd3DofModeEnabled},
    {"ovrp_GetSystemRegion2", (void *)klovrp_GetSystemRegion2},
    {"ovrp_GetSystemVSyncCount2", (void *)klovrp_GetSystemVSyncCount2},
    {"ovrp_GetTiledMultiResDynamic", (void *)klovrp_GetTiledMultiResDynamic},
    {"ovrp_GetTimeInSeconds", (void *)klovrp_GetTimeInSeconds},
    {"ovrp_GetTrackingCalibratedOrigin2", (void *)klovrp_GetTrackingCalibratedOrigin2},
    {"ovrp_GetTrackingCalibratedOrigin", (void *)klovrp_GetTrackingCalibratedOrigin_entry},
    {"ovrp_GetTrackingIPDEnabled2", (void *)klovrp_GetTrackingIPDEnabled2},
    {"ovrp_GetTrackingOrientationEnabled2", (void *)klovrp_GetTrackingOrientationEnabled2},
    {"ovrp_GetTrackingOrientationSupported2", (void *)klovrp_GetTrackingOrientationSupported2},
    {"ovrp_GetTrackingPositionEnabled2", (void *)klovrp_GetTrackingPositionEnabled2},
    {"ovrp_GetTrackingTransformRawPose", (void *)klovrp_GetTrackingTransformRawPose},
    {"ovrp_GetUserEyeHeight", (void *)klovrp_GetUserEyeHeight},
    {"ovrp_InitializeEnvironmentDepth", (void *)klovrp_InitializeEnvironmentDepth},
    {"ovrp_IsLayerShapeSupported", (void *)klovrp_IsLayerShapeSupported},
    {"ovrp_IsRequestingASWData", (void *)klovrp_IsRequestingASWData},
    {"ovrp_Media_GetMrcActivationMode", (void *)klovrp_Media_GetMrcActivationMode},
    {"ovrp_Media_GetMrcAudioSampleRate", (void *)klovrp_Media_GetMrcAudioSampleRate},
    {"ovrp_Media_GetMrcFrameImageFlipped", (void *)klovrp_Media_GetMrcFrameImageFlipped},
    {"ovrp_Media_GetMrcFrameInverseAlpha", (void *)klovrp_Media_GetMrcFrameInverseAlpha},
    {"ovrp_Media_GetMrcFrameSize", (void *)klovrp_Media_GetMrcFrameSize},
    {"ovrp_Media_IsCastingToRemoteClient", (void *)klovrp_Media_IsCastingToRemoteClient},
    {"ovrp_Media_IsMrcActivated", (void *)klovrp_Media_IsMrcActivated},
    {"ovrp_Media_IsMrcEnabled", (void *)klovrp_Media_IsMrcEnabled},
    {"ovrp_Media_SetMrcFrameSize", (void *)klovrp_Media_SetMrcFrameSize},
    {"ovrp_Media_Shutdown", (void *)klovrp_Media_Shutdown},
    {"ovrp_Media_UseMrcDebugCamera", (void *)klovrp_Media_UseMrcDebugCamera},
    {"ovrp_SetEnvironmentDepthHandRemoval", (void *)klovrp_SetEnvironmentDepthHandRemoval},
    {"ovrp_SetHeadPoseModifier", (void *)klovrp_SetHeadPoseModifier},
    {"ovrp_SetInsightPassthroughKeyboardHandsIntensity", (void *)klovrp_SetInsightPassthroughKeyboardHandsIntensity},
    {"ovrp_StartBodyTracking", (void *)klovrp_body_unsupported},
    {"ovrp_StartEnvironmentDepth", (void *)klovrp_StartEnvironmentDepth},
    {"ovrp_StopEnvironmentDepth", (void *)klovrp_StopEnvironmentDepth},
    {"ovrp_UpdatePassthroughColorLut", (void *)klovrp_UpdatePassthroughColorLut},
    {"UnityPluginLoad",   (void *)klovrp_UnityPluginLoad},
    {"ovrp_GetUnityInterfaces", (void *)klovrp_GetUnityInterfaces},
    {"UnityPluginUnload", (void *)klovrp_UnityPluginUnload},
    {"JNI_OnLoad",        (void *)klovrp_JNI_OnLoad},
    {"ovrp_GetSkeleton2", (void *)klovrp_GetSkeleton2},
    // The OVRP_1_44_0 hand surface, answered while hand tracking is on for the
    // guest (kl_ovrp_set_hand_joints) and refused otherwise; the newer shapes
    // refused as a group. See klovrp_hand_unsupported for why one handler may
    // serve numbered siblings here when it may not anywhere else in this file.
    {"ovrp_GetSkeleton",  (void *)klovrp_GetSkeleton},
    {"ovrp_GetSkeleton3", (void *)klovrp_GetSkeleton3},
    {"ovrp_GetControllerIsInHand", (void *)klovrp_GetControllerIsInHand},
    {"ovrp_GetHandTrackingEnabled", (void *)klovrp_GetHandTrackingEnabled},
    {"ovrp_GetHandState",  (void *)klovrp_GetHandState},
    {"ovrp_GetHandState2", (void *)klovrp_GetHandState2},
    // Wooorld: asked once libovrplugintracking loads (the Avatar SDK's tracking
    // bridge), which it does now that its TLS relocations are served.
    {"ovrp_GetHandState3", (void *)klovrp_hand_unsupported},
    {"ovrp_GetMesh",  (void *)klovrp_GetMesh},
    {"ovrp_GetInstanceExtensionsVk", (void *)klovrp_GetInstanceExtensionsVk},
    {"ovrp_GetDeviceExtensionsVk",   (void *)klovrp_GetDeviceExtensionsVk},
    // The init state and everything that reads it. See klovrp_GetInitialized.
    {"ovrp_Initialize",    (void *)klovrp_Initialize1},
    {"ovrp_Initialize2",   (void *)klovrp_Initialize2},
    {"ovrp_Initialize3",   (void *)klovrp_Initialize3},
    {"ovrp_Initialize4",   (void *)klovrp_Initialize4},
    {"ovrp_Initialize5",   (void *)klovrp_Initialize5},
    {"ovrp_Initialize6",   (void *)klovrp_Initialize6},
    {"ovrp_RecenterTrackingOrigin2", (void *)klovrp_RecenterTrackingOrigin2},
    {"ovrp_GetAudioOutId2",       (void *)klovrp_GetAudioOutId2},
    {"ovrp_GetAudioInId2",        (void *)klovrp_GetAudioInId2},
    {"ovrp_GetDisplayAdapterId2", (void *)klovrp_GetDisplayAdapterId2},
    {"ovrp_Initialize7",   (void *)klovrp_Initialize7},
    {"ovrp_GetInitialized", (void *)klovrp_GetInitialized},
    {"ovrp_Shutdown",      (void *)klovrp_Shutdown},
    {"ovrp_Shutdown2",     (void *)klovrp_Shutdown2},
    {"ovrp_GetAppChromaticCorrection", (void *)klovrp_GetAppChromaticCorrection},
    {"ovrp_SetAppChromaticCorrection", (void *)klovrp_SetAppChromaticCorrection},
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
    {"ovrp_GetVersion",   (void *)klovrp_GetVersion},
    {"ovrp_GetVersion2",   (void *)klovrp_GetVersion2},
    {"ovrp_AllowVisibilityMask",              (void *)klovrp_AllowVisibilityMask},
    {"ovrp_SetExternalLayerDynresEnabled",    (void *)klovrp_SetExternalLayerDynresEnabled},
    {"ovrp_GetEyeLayerRecommendedResolution", (void *)klovrp_GetEyeLayerRecommendedResolution},
    {"ovrp_UnityOpenXR_SetClientVersion",        (void *)klovrp_UOX_SetClientVersion},
    {"ovrp_UnityOpenXR_HookGetInstanceProcAddr", (void *)klovrp_UOX_HookGetInstanceProcAddr},
    {"ovrp_UnityOpenXR_OnInstanceCreate",        (void *)klovrp_UOX_OnInstanceCreate},
    {"ovrp_UnityOpenXR_OnInstanceDestroy",       (void *)klovrp_UOX_OnInstanceDestroy},
    {"ovrp_UnityOpenXR_OnSessionCreate",         (void *)klovrp_UOX_OnSessionCreate},
    {"ovrp_UnityOpenXR_OnSessionBegin",          (void *)klovrp_UOX_OnSessionBegin},
    {"ovrp_UnityOpenXR_OnSessionEnd",            (void *)klovrp_UOX_OnSessionEnd},
    {"ovrp_UnityOpenXR_OnSessionExiting",        (void *)klovrp_UOX_OnSessionExiting},
    {"ovrp_UnityOpenXR_OnSessionDestroy",        (void *)klovrp_UOX_OnSessionDestroy},
    {"ovrp_UnityOpenXR_OnAppSpaceChange",        (void *)klovrp_UOX_OnAppSpaceChange},
    {"ovrp_UnityOpenXR_OnAppSpaceChange2",       (void *)klovrp_UOX_OnAppSpaceChange2},
    {"ovrp_GetAppSpace",                   (void *)klovrp_GetAppSpace},
    {"ovrp_RegisterOpenXREventHandler",    (void *)klovrp_RegisterOpenXREventHandler},
    {"ovrp_UnregisterOpenXREventHandler",  (void *)klovrp_UnregisterOpenXREventHandler},
    {"ovrp_GetNativeOpenXRHandles",        (void *)klovrp_GetNativeOpenXRHandles},
    {"ovrp_GetOpenXRInstanceProcAddrFunc", (void *)klovrp_GetOpenXRInstanceProcAddrFunc},
    {"ovrp_TelemetryCreateMetadataHandle", (void *)klovrp_TelemetryCreateMetadataHandle},
    {"ovrp_GetRuntimeName",                (void *)klovrp_GetRuntimeName},
    {"ovrp_GetHandPoseSourceInferred",     (void *)klovrp_GetHandPoseSourceInferred},
    {"ovrp_UnityOpenXR_OnSessionStateChange",    (void *)klovrp_UOX_OnSessionStateChange},
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
    {"ovrp_GetTrackingPositionSupported2", (void *)klovrp_GetTrackingPositionSupported2},
    {"ovrp_GetEyeTextureArraySupported",  (void *)klovrp_GetEyeTextureArraySupported},
    {"ovrp_GetBoundaryConfigured2", (void *)klovrp_GetBoundaryConfigured2},
    {"ovrp_GetAppHasVrFocus2", (void *)klovrp_GetAppHasVrFocus2},
    {"ovrp_GetUserPresent2", (void *)klovrp_GetUserPresent2},
    {"ovrp_GetNodePresent2", (void *)klovrp_GetNodePresent2},
    {"ovrp_GetEnvironmentDepthFrameDesc", (void *)klovrp_GetEnvironmentDepthFrameDesc},
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
    {"ovrp_GetTrackingTransformRelativePose", (void *)klovrp_GetTrackingTransformRelativePose},
    {"ovrp_PollEvent",  (void *)klovrp_PollEvent},
    {"ovrp_PollEvent2", (void *)klovrp_PollEvent2},
    // The emulated room (KL_OVRP_SCENE_ROOM); UNSUPPORTED where it is off.
    {"ovrp_DiscoverSpaces", (void *)klovrp_DiscoverSpaces},
    {"ovrp_RetrieveSpaceDiscoveryResults", (void *)klovrp_RetrieveSpaceDiscoveryResults},
    {"ovrp_GetSpaceUuid", (void *)klovrp_GetSpaceUuid},
    {"ovrp_EnumerateSpaceSupportedComponents", (void *)klovrp_EnumerateSpaceSupportedComponents},
    {"ovrp_GetSpaceComponentStatus", (void *)klovrp_GetSpaceComponentStatus},
    {"ovrp_SetSpaceComponentStatus", (void *)klovrp_SetSpaceComponentStatus},
    {"ovrp_LocateSpace2", (void *)klovrp_LocateSpace2},
    {"ovrp_LocateSpace", (void *)klovrp_LocateSpace},
    {"ovrp_GetSpaceRoomLayout", (void *)klovrp_GetSpaceRoomLayout},
    {"ovrp_GetSpaceContainer", (void *)klovrp_GetSpaceContainer},
    {"ovrp_GetSpaceSemanticLabels", (void *)klovrp_GetSpaceSemanticLabels},
    {"ovrp_GetSpaceBoundingBox2D", (void *)klovrp_GetSpaceBoundingBox2D},
    {"ovrp_GetSpaceBoundary2D", (void *)klovrp_GetSpaceBoundary2D},
    {"ovrp_GetSpaceBoundingBox3D", (void *)klovrp_GetSpaceBoundingBox3D},
    {"ovrp_DestroySpace", (void *)klovrp_DestroySpace},
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
    {"ovrp_SetColorScaleAndOffset", (void *)klovrp_SetColorScaleAndOffset},
    {"ovrp_EnqueueSetupLayer2", (void *)klovrp_EnqueueSetupLayer2},
    {"ovrp_EnqueueSubmitLayer2", (void *)klovrp_EnqueueSubmitLayer2},
    {"ovrp_SetOverlayQuad3", (void *)klovrp_SetOverlayQuad3},
    {"ovrp_DestroyLayer", (void *)klovrp_DestroyLayer},
    {"ovrp_EnqueueDestroyLayer", (void *)klovrp_EnqueueDestroyLayer},
    {"ovrp_GetLayerTextureStageCount", (void *)klovrp_GetLayerTextureStageCount},
    {"ovrp_GetLayerRecommendedResolution", (void *)klovrp_GetLayerRecommendedResolution},
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
    {"ovrp_GetSystemCpuLevel", (void *)klovrp_GetSystemCpuLevel},
    {"ovrp_GetSystemGpuLevel", (void *)klovrp_GetSystemGpuLevel},
    {"ovrp_CalculateEyeViewportRect", (void *)klovrp_CalculateEyeViewportRect},
    {"ovrp_CalculateEyePreviewRect", (void *)klovrp_CalculateEyePreviewRect},
    {"ovrp_GetAppPerfStats", (void *)klovrp_GetAppPerfStats},
    {"ovrp_GetBoundaryDimensions", (void *)klovrp_GetBoundaryDimensions},
    {"ovrp_GetControllerState2", (void *)klovrp_GetControllerState2_entry},
    {"ovrp_GetControllerState", (void *)klovrp_GetControllerState_entry},
    {"ovrp_GetControllerState4", (void *)klovrp_GetControllerState4},
    {"ovrp_GetControllerState5", (void *)klovrp_GetControllerState5},
    {"ovrp_GetControllerState6", (void *)klovrp_GetControllerState6},
    {"ovrp_GetDominantHand", (void *)klovrp_GetDominantHand},
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
    {"ovrp_GetEyeTrackingSupported", (void *)klovrp_GetEyeTrackingSupported},
    {"ovrp_GetEyeTrackingEnabled", (void *)klovrp_GetEyeTrackingEnabled},
    {"ovrp_GetBodyTrackingSupported", (void *)klovrp_GetBodyTrackingSupported},
    {"ovrp_GetBodyTrackingEnabled", (void *)klovrp_GetBodyTrackingEnabled},
    {"ovrp_GetFullBodyTrackingEnabled", (void *)klovrp_GetFullBodyTrackingEnabled},
    {"ovrp_GetFaceTrackingSupported", (void *)klovrp_GetFaceTrackingSupported},
    {"ovrp_GetFaceTrackingEnabled", (void *)klovrp_GetFaceTrackingEnabled},
    {"ovrp_GetMixedRealityInitialized", (void *)klovrp_GetMixedRealityInitialized},
    {"ovrp_GetPassthroughCapabilityFlags", (void *)klovrp_GetPassthroughCapabilityFlags},
    // Insight Passthrough — see klovrp_pt_on.
    {"ovrp_InitializeInsightPassthrough", (void *)klovrp_InitializeInsightPassthrough},
    {"ovrp_ShutdownInsightPassthrough", (void *)klovrp_ShutdownInsightPassthrough},
    {"ovrp_GetInsightPassthroughInitialized", (void *)klovrp_GetInsightPassthroughInitialized},
    {"ovrp_GetInsightPassthroughInitializationState",
     (void *)klovrp_GetInsightPassthroughInitializationState},
    {"ovrp_SetInsightPassthroughStyle", (void *)klovrp_SetInsightPassthroughStyle},
    {"ovrp_CreateInsightTriangleMesh", (void *)klovrp_CreateInsightTriangleMesh},
    {"ovrp_DestroyInsightTriangleMesh", (void *)klovrp_DestroyInsightTriangleMesh},
    {"ovrp_AddInsightPassthroughSurfaceGeometry",
     (void *)klovrp_AddInsightPassthroughSurfaceGeometry},
    {"ovrp_DestroyInsightPassthroughGeometryInstance",
     (void *)klovrp_DestroyInsightPassthroughGeometryInstance},
    {"ovrp_UpdateInsightPassthroughGeometryTransform",
     (void *)klovrp_UpdateInsightPassthroughGeometryTransform},
    {"ovrp_IsInsightPassthroughSupported", (void *)klovrp_IsInsightPassthroughSupported},
    {"ovrp_IsControllerDrivenHandPosesEnabled", (void *)klovrp_IsControllerDrivenHandPosesEnabled},
    {"ovrp_GetPassthroughCapabilities", (void *)klovrp_GetPassthroughCapabilities},
    {"ovrp_GetPassthroughPreferences", (void *)klovrp_GetPassthroughPreferences},
    {"ovrp_SetInsightPassthroughStyle2", (void *)klovrp_SetInsightPassthroughStyle2},
    {"ovrp_GetBoundaryVisibility", (void *)klovrp_GetBoundaryVisibility},
    {"ovrp_RequestBoundaryVisibility", (void *)klovrp_RequestBoundaryVisibility},
    {"ovrp_GetLocalDimmingSupported", (void *)klovrp_GetLocalDimmingSupported},
    {"ovrp_IsMultimodalHandsControllersSupported",
     (void *)klovrp_IsMultimodalHandsControllersSupported},
    {"ovrp_GetEyeFovPremultipliedAlphaMode", (void *)klovrp_GetEyeFovPremultipliedAlphaMode},
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
    // Meta's OpenXR-backend startup (OVRPlugin 1.203.0, DreamSpace): each is
    // `ovrpResult f(scalar...)` in the real plugin — a stored field or a forward,
    // no out-parameter — so record-and-succeed. The telemetry pair goes with
    // ovrp_TelemetryCreateMetadataHandle (no telemetry pipe here).
    "ovrp_SetHandSkeletonVersion", "ovrp_SetSimultaneousHandsAndControllersEnabled",
    "ovrp_SetEyeBufferSecure", "ovrp_TelemetrySetMetadata", "ovrp_SendUnifiedEventV5",
    // ...and the rest of the telemetry sink, as a family: in the real 1.203.0
    // plugin every setter is `ovrpResult f(handle, key, value...)` — a call into
    // EngineTelemetry and `csel w0, wzr, -1000` — and every SendUnifiedEvent
    // revision only takes arguments. MR Utility Kit calls SetMetadataInt once
    // the room has loaded. (TelemetryGetMetadata writes an answer back and is
    // not here.)
    "ovrp_TelemetrySetMetadataBool", "ovrp_TelemetrySetMetadataInt",
    "ovrp_TelemetrySetMetadataLong", "ovrp_TelemetrySetMetadataFloat",
    "ovrp_TelemetrySetMetadataDouble", "ovrp_TelemetrySetMetadataIntArray",
    "ovrp_TelemetrySetMetadataLongArray", "ovrp_TelemetrySetMetadataDoubleArray",
    "ovrp_TelemetrySetMetadataStringArray",
    "ovrp_SendUnifiedEvent", "ovrp_SendUnifiedEventV2", "ovrp_SendUnifiedEventV3",
    "ovrp_SendUnifiedEventV4",
    // Shutdown-callback registration (real 1.203.0: hand the pointer to the
    // backend, return 0). No shutdown is ever announced here, so accepting the
    // registration is the whole contract. (The OpenXR-event pair has a real body:
    // klovrp_RegisterOpenXREventHandler.)
    "ovrp_RegisterShutdownEventHandler", "ovrp_UnregisterShutdownEventHandler",
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
    // ...and the three a 1.109 guest adds (Demeo).
    "ovrp_QplMarkerAnnotationVariant", "ovrp_QplMarkerPointData", "ovrp_QplMarkerStartForJoin",
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
    // (ovrp_GetEnvironmentDepthFrameDesc was here; it has to write its answer —
    // see klovrp_GetEnvironmentDepthFrameDesc.)
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
    // (ovrp_SetColorScaleAndOffset was here, recorded and dropped; it has a real
    // body now — klovrp_SetColorScaleAndOffset.)
    // The OVERLAY submit path. AC Nexus drives its main eye layer through
    // BeginFrame4/EndFrame4 (implemented, and where the eye texture is captured)
    // but submits its Equirect background OVROverlay through the older enqueue
    // model. Accepting it as a no-op drops only that background layer - the eye
    // projection still reaches the compositor through EndFrame4 - so it is
    // record-and-succeed rather than a real per-layer submit. If the background
    // turns out to matter, it graduates to a real body that records the overlay.
    // Premultiplied-alpha mode for the eye-FOV layer. Some guests push it
    // during OVRPlugin setup. ovrpResult/record-and-succeed: the compositor
    // here composites the eye projection through EndFrame4 and does not honour a
    // per-layer alpha-blend mode, so accepting the push keeps the guest moving
    // without changing what reaches the display. Graduates to a real body only
    // if a title turns out to read the alpha mode back and require it.
    "ovrp_SetEyeFovPremultipliedAlphaMode",
    // Whether the compositor masks the lens' hidden area with the occlusion
    // mesh. ovrpResult (the C# wrapper compares it to Result.Success).
    // There is no occlusion mesh on this host — ovrp_GetEyeOcclusionMesh and
    // ovrp_GetViewportStencil already say so — so the switch has nothing to
    // act on; accepting it is recorded state, like the other setters here.
    // Keep Talking and Nobody Explodes pushes it while OVRManager starts up.
    "ovrp_SetEyeOcclusionMeshEnabled",
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
    // No fixed-foveated/tiled multires rendering in our GL gateway.
    "ovrp_GetTiledMultiResSupported",
};

static const char *const g_ovrp_result_unsupported[] = {
    // Hand-tracking state, with the rest of the hand group (DreamSpace asks).
    "ovrp_GetHandTrackingState",
    // Simultaneous Quest controller/hand mode is not implemented on this host.
    "ovrp_SetSimultaneousHandsAndControllersEnabled",
    "ovrp_AreHandPosesGeneratedByControllerData",
    // (Insight Passthrough was here — init, its state and the layer style. They
    // have answers of their own now, which are still UNSUPPORTED for every
    // guest passthrough is not offered to: klovrp_pt_on.)
    // Parametric haptics: a newer OVRPlugin's amplitude/frequency curves, next to
    // the buffered and PCM paths answered further up. Marvel's Deadpool VR asks
    // for the controller's parametric properties while it sets haptics up, and
    // with no entry point the game stopped there, two seconds in (2026-10-09).
    // Nothing here plays a parametric effect, so UNSUPPORTED with the out-struct
    // untouched is the truthful answer — the one a headset without the feature
    // gives — and the guest keeps to the haptics paths that are real.
    "ovrp_GetControllerParametricProperties",
    // Body and face tracking, the calls behind the supported/enabled questions
    // answered above. Deadpool VR reads ovrp_GetBodyState4 every frame once it is
    // in the game even though it was told body tracking is neither supported nor
    // enabled, and with no entry point it stopped there (2026-10-09). The state
    // struct is large and its layout is not something to guess at, so it is left
    // untouched and the call says UNSUPPORTED — which Meta's Unreal plugin treats
    // as "no body state this frame". (Not NOT_YET_IMPLEMENTED: that one trips an
    // ensure in the same plugin.) Starting, stopping and tuning what does not
    // exist get the same answer.
    "ovrp_GetBodyState4",
    "ovrp_StartBodyTracking2",
    "ovrp_StopBodyTracking",
    "ovrp_RequestBodyTrackingFidelity",
    "ovrp_ResetBodyTrackingCalibration",
    "ovrp_SuggestBodyTrackingCalibrationOverride",
    "ovrp_GetFaceState",
    "ovrp_StartFaceTracking",
    "ovrp_StopFaceTracking",
};

// Names taken over from rebelancap's fork (round 4) that this file did not
// know at all, with the generic answer his lists give them: spaces, anchors
// and scene capture are unsupported (-1004); setters with nothing behind them
// succeed; the Enqueue-ABI layer calls this gateway does not serve and the
// mixed-reality capture getters are refused (-1000). Kept apart from the lists
// above so that what this file answered before, and how, can still be read off
// them.
static const char *const g_ovrp_r4_unsupported[] = {
    "ovrp_LocateSpace",
    "ovrp_CreateSpatialAnchor",
    "ovrp_DestroySpace",
    "ovrp_SetSpaceComponentStatus",
    "ovrp_GetSpaceComponentStatus",
    "ovrp_EnumerateSpaceSupportedComponents",
    "ovrp_QuerySpaces",
    "ovrp_RetrieveSpaceQueryResults",
    "ovrp_SaveSpace",
    "ovrp_EraseSpace",
    "ovrp_GetSpaceUuid",
    "ovrp_SaveSpaceList",
    "ovrp_ShareSpaces",
    "ovrp_CreateSpaceUser",
    "ovrp_DestroySpaceUser",
    "ovrp_GetSpaceContainer",
    "ovrp_GetSpaceBoundingBox2D",
    "ovrp_GetSpaceBoundingBox3D",
    "ovrp_GetSpaceSemanticLabels",
    "ovrp_GetSpaceRoomLayout",
    "ovrp_GetSpaceBoundary2D",
    "ovrp_RequestSceneCapture",
    "ovrp_ShutdownMixedReality",
    "ovrp_GetFoveationEyeTrackedCenter",
    "ovrp_GetHandNodePoseStateLatency",
    "ovrp_SetHandNodePoseStateLatency",
    "ovrp_GetGPUUtilLevel",
    "ovrp_GetBoundaryGeometry3",
    "ovrp_TestBoundaryNode2",
    "ovrp_TestBoundaryPoint2",
    "ovrp_SetupMirrorTexture2",
    "ovrp_DestroyMirrorTexture2",
    "ovrp_GetLayerOcclusionMesh",
    "ovrp_GetAppLatencyTimings2",
    "ovrp_ShowSystemUI2",
};
static const char *const g_ovrp_r4_ok[] = {
    "ovrp_AddCustomMetadata",
    "ovrp_SetDeveloperMode",
    "ovrp_SetASWAdaptiveMode",
    "ovrp_SetRemoteHandedness",
    "ovrp_ResetAppPerfStats2",
    "ovrp_SetControllerHaptics2",
    "ovrp_SetControllerHapticsAmplitudeEnvelope",
    "ovrp_UpdateFoveation",
    "ovrp_SetTrackingPositionEnabled2",
    "ovrp_SetTrackingOrientationEnabled2",
    "ovrp_SetTrackingIPDEnabled2",
    "ovrp_SetNodePositionTracked2",
    "ovrp_SetBoundaryVisible2",
    "ovrp_SetInhibitSystemUX2",
    "ovrp_SetSystemVSyncCount2",
    "ovrp_SetUserEyeHeight2",
    "ovrp_SetUserIPD2",
    "ovrp_SetTrackingCalibratedOrigin2",
    "ovrp_Media_SetPlatformInitialized",
    "ovrp_Media_Update",
    "ovrp_Media_SetHeadsetControllerPose",
    "ovrp_Media_SetCameraMinMaxDistance",
    "ovrp_Media_SetCustomCameraAnchorPose",
    "ovrp_Media_SetPlatformCameraMode",
    "ovrp_ResetDefaultExternalCamera",
    "ovrp_UpdateExternalCamera",
    "ovrp_SetDefaultExternalCamera",
};
static const char *const g_ovrp_r4_refused[] = {
    "ovrp_EnqueueSetupLayer",
    "ovrp_EnqueueSubmitLayer",
    "ovrp_SetupLayerDepth",
    "ovrp_GetEyeFovLayerId",
    "ovrp_Media_GetPlatformCameraMode",
    "ovrp_Media_EncodeMrcFrame",
    "ovrp_Media_EncodeMrcFrameWithPoseTime",
    "ovrp_Media_EncodeMrcFrameWithDualTextures",
    "ovrp_Media_EncodeMrcFrameDualTexturesWithPoseTime",
    "ovrp_Media_SyncMrcFrame",
    "ovrp_Media_CreateCustomCameraAnchor",
    "ovrp_Media_DestroyCustomCameraAnchor",
    "ovrp_Media_EnumerateCameraAnchorHandles",
    "ovrp_Media_GetCameraAnchorHandle",
    "ovrp_Media_GetCameraAnchorName",
    "ovrp_Media_GetCameraAnchorType",
    "ovrp_Media_GetCurrentCameraAnchorHandle",
    "ovrp_Media_GetCustomCameraAnchorPose",
    "ovrp_Media_GetCameraMinMaxDistance",
    "ovrp_GetExternalCameraCount",
    "ovrp_GetExternalCameraName",
    "ovrp_GetExternalCameraIntrinsics",
    "ovrp_GetExternalCameraExtrinsics",
    "ovrp_GetExternalCameraCalibrationRawPose",
    "ovrp_GetExternalCameraPose",
    "ovrp_GetUseOverriddenExternalCameraFov",
    "ovrp_GetUseOverriddenExternalCameraStaticPose",
    "ovrp_OverrideExternalCameraFov",
    "ovrp_OverrideExternalCameraStaticPose",
    "ovrp_SetExternalCameraProperties",
};

static void *klovrp_shared(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_r4_unsupported / sizeof g_ovrp_r4_unsupported[0]; i++)
        if (strcmp(g_ovrp_r4_unsupported[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_unsupported);
    for (size_t i = 0; i < sizeof g_ovrp_r4_ok / sizeof g_ovrp_r4_ok[0]; i++)
        if (strcmp(g_ovrp_r4_ok[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_ok);
    for (size_t i = 0; i < sizeof g_ovrp_r4_refused / sizeof g_ovrp_r4_refused[0]; i++)
        if (strcmp(g_ovrp_r4_refused[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_refuse);
    for (size_t i = 0; i < sizeof g_ovrp_result_ok / sizeof g_ovrp_result_ok[0]; i++)
        if (strcmp(g_ovrp_result_ok[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_ok);
    for (size_t i = 0; i < sizeof g_ovrp_result_unsupported / sizeof g_ovrp_result_unsupported[0]; i++)
        if (strcmp(g_ovrp_result_unsupported[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_unsupported);
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

// Entry points this plugin reports as NOT EXPORTED rather than as named stubs.
// Only for callers that probe by dlsym and have their own fallback for a NULL:
// a stub there reads as "present", and the first call aborts.
//
// The Haptics SDK (1.0.0-beta.2, Cave Crave) resolves the parametric pair and,
// if both load, prefers its parametric driver over PCM ("Capabilities of
// OVRPlugin backend: Parametric: true, PCM: true"). Parametric streaming is
// not implemented here; PCM is (SetControllerHapticsPcm above). With these
// two absent it logs "Unable to load ..." and drives the controllers over PCM
// — the same path Beat Saber's copy of the SDK takes.
static const char *const g_ovrp_absent[] = {
    "ovrp_SetControllerHapticsParametric",
    "ovrp_GetControllerParametricStreamingTimingProperties",
};

// Answers in g_ovrp_impl that only one title has been seen to need. Every
// other target resolves these names as before: the named stub, which aborts
// with the name (or says UNSUPPORTED under KL_OVRP_UNANSWERED).
//   ovrp_GetTrackingCalibratedOrigin — Onward (x2677344882310094), fix 50
//   ovrp_GetBoundaryConfigured/Geometry2/Geometry3 — KL_OVRP_BOUNDARY
static int kl_ovrp_gated_out(const char *name) {
    // The Guardian under KL_OVRP_BOUNDARY: off, these keep their old answers
    // from the name lists below ("no boundary").
    if (!strcmp(name, "ovrp_GetBoundaryConfigured") || !strcmp(name, "ovrp_GetBoundaryGeometry2") ||
        !strcmp(name, "ovrp_GetBoundaryGeometry3"))
        return !klovrp_boundary(NULL);
    if (strcmp(name, "ovrp_GetTrackingCalibratedOrigin") == 0) {
        const char *t = kl_driver_target_name();
        return !t || strcmp(t, "x2677344882310094") != 0;
    }
    return 0;
}

static void *kl_ovrp_sym_inner(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_impl / sizeof g_ovrp_impl[0]; i++)
        if (strcmp(g_ovrp_impl[i].name, name) == 0) {
            if (kl_ovrp_gated_out(name)) break;
            return g_ovrp_impl[i].fn;
        }
    for (size_t i = 0; i < sizeof g_ovrp_absent / sizeof g_ovrp_absent[0]; i++)
        if (strcmp(g_ovrp_absent[i], name) == 0) return NULL;
    // Unity IL2CPP P/Invoke probes the "W" (wide) variant first for entry points
    // with CharSet.Auto. Android's libOVRPlugin.so does not export "W" entry points
    // (except ovrp_DismissHSW). Returning NULL lets IL2CPP fall back to the
    // un-suffixed symbol.
    size_t n = strlen(name);
    if (n > 1 && name[n - 1] == 'W' && strcmp(name, "ovrp_DismissHSW") != 0)
        return NULL;
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
    // The eye swapchain. Frames should be spread across the stages: all of them
    // on one stage means the guest is not cycling and the compositor is reading
    // the texture the guest is writing (see KLOVRP_STAGES_DEFAULT).
    pthread_mutex_lock(&g_frames.mu);
    uint64_t serial = g_frames.serial, guessed = g_frames.guessed;
    uint64_t unobserved = g_frames.unobserved, multi = g_frames.multi;
    uint64_t cross = g_frames.cross_thread, disagree = g_frames.stage_disagree;
    uint64_t named = g_frames.named, named_dis = g_frames.named_disagree;
    uint64_t repeat_drop = g_frames.repeat_drop;
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
        // Frames the guest submitted without rotating its stage — a dropped
        // frame, on a guest the GL observation cannot see. Their records are
        // NOT filed, so the compositor keeps showing the previous picture with
        // the pose it was drawn at instead of pairing it with a newer one.
        if (repeat_drop)
            fprintf(f, "  ...and %llu frame(s) named the SAME stage again and were "
                       "dropped as having drawn nothing (KL_OVRP_DROP_REPEAT=0 "
                       "files them anyway)\n",
                    (unsigned long long)repeat_drop);
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
