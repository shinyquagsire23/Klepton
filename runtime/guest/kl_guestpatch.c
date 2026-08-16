// Measured guest-image patches — the table, and the guard that makes it safe.
//
// Everything here exists because of one class of gate: a branch inside the
// guest that this host cannot answer from outside. The project's rule is to
// implement what the guest FORCES and to answer honestly; a patch is the
// admission that there is no question being asked of us at all — the decision
// is already made inside the binary, and the only way to observe what is behind
// it is to take the other edge.
//
// So the bar for a row here is high, and every row carries how it was MEASURED:
//
//   * the address is a guest vaddr in a named library, not an offset in a file;
//   * the `expect` word is the instruction that was disassembled there, which
//     doubles as a build fingerprint — a different APK almost never has the
//     same word at the same address, so a stale row is skipped and NAMED rather
//     than corrupting an image;
//   * a group is atomic: either every word matches and all are written, or none
//     are. Half of a two-instruction replacement is a crash, not a no-op.
//
// Knobs (DEBUG_ENV_VARS.md): `KL_GUEST_PATCH=0` turns all of this off, which is
// the A/B for every row; `KL_GUEST_PATCH_OFF=<name>[,<name>...]` turns off one.
//
// DEVICE: a klepton-ld dylib is signed, so its text cannot be written at run
// time — which is why the entry point takes a RESOLVER rather than a base
// pointer, so the same table can be applied offline. tools/klepton_ld.c does
// exactly that (see its `gp_at`), so a row here reaches the headset as well as
// the host, and there is no host/device divergence to remember.
#include "kl_guestpatch.h"
#include "kl_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t va; uint32_t expect, to; const char *what; } kl_gp_word;

typedef struct {
    const char       *name;      // the knob name, and what a log line calls it
    const char       *lib;       // basename of the guest library
    const char       *why;       // one line, printed when it is applied
    const kl_gp_word *words;
    unsigned          nwords;
} kl_gp_patch;

// ---------------------------------------------------------------------------
// VRChat (Steam Frame build, `Assembly-CSharp.dll` inside libil2cpp.so).
//
// Both rows were reached the same way and the route is worth keeping, because
// nothing in this target has a symbol: the metadata is decrypted with
// tools/vrc_metadata.py, an `Il2CppCodeGenModule`'s methodPointers array turns
// a method's token RID into an address, and a v29 metadata-usage slot holds
// `(kind << 29) | (index << 1) | 1` in the FILE until it is initialised — so a
// string literal decodes back to the `adrp`+`ldr` that reads it, and from there
// to the method.

// (1) The "Under Construction" wall.
//
// `VRCFlowManager`'s ManageGameFlow coroutine does, in effect:
//
//     while (VRCUiManager.Instance == null) yield return null;   // 'waiting for UI Manager'
//     yield return this.<wall>();                                // <-- here
//
// and the wall coroutine it yields is:
//
//     while (VRCUiPopupManager.Instance == null) yield return null;
//     VRCUiPopupManager.Instance.<hide>(null);
//     VRCUiManager.Instance.ShowScreen("MenuContent/Screens/Authentication/UnderConstruction");
//     for (;;) yield return null;                                // never completes
//
// The last line is what makes this a wall rather than a screen: the outer
// coroutine is waiting on an inner one that never returns false, so the whole
// game flow stops there forever. It is also why the obvious patches do NOT
// work, and both were tried: aliasing the screen-path string literal to
// `.../LoginPrompt` shows a different screen and still never advances, and
// NOPing the one conditional branch that reaches the yield leaves the other
// predecessor — the paths converge, so in this build the wall is effectively
// unconditional.
//
// What works is to make the inner coroutine COMPLETE: `MoveNext` returns false
// on its first call, `yield return` finishes immediately, and the flow carries
// on to what was behind it. The screen is never shown, and nothing else in the
// coroutine has a side effect worth keeping.
//
//   0x698d634  str x30, [sp, #-0x30]!   -> mov w0, #0   ; "the enumerator is done"
//   0x698d638  stp x22, x21, [sp, #0x10] -> ret
//
// The method was confirmed to RUN before it was patched, by writing a `brk #0`
// at its ShowScreen call and watching the process die on signal 5 — worth the
// extra run, because the two failed patches above look exactly like dead code.
static const kl_gp_word k_vrc_wall[] = {
    { 0x698d634, 0xf81d0ffe, 0x52800000, "mov w0, #0  (MoveNext -> false)" },
    { 0x698d638, 0xa90157f6, 0xd65f03c0, "ret" },
};

// (2) The minimum-client-version screen, which is what is immediately behind
// the wall: "Update Required — Current: 1862, Required: 1865".
//
// This one is a real check against a real answer — the required build number
// comes from VRChat's own API — and the honest fix is a newer APK: the tree
// here is three builds old. It is patched anyway because otherwise the target
// stops at a second dead screen and nothing downstream of it can be brought up
// at all, and because the number it compares is not a measurement this host can
// make. Turn it off with `KL_GUEST_PATCH_OFF=vrchat-min-client-version` to see
// the screen again.
//
//   0x696828c  ldp w8, w9, [x0, #0x10]  ; current, required
//   0x6968290  cmp w8, w9
//   0x6968294  b.ge 0x6968350           -> b 0x6968350   ; always "new enough"
static const kl_gp_word k_vrc_minver[] = {
    { 0x6968294, 0x540005ea, 0x1400002f, "b.ge -> b  (skip VerifyMinClientVersion)" },
};

// (3) The "Going to Error World" transition.
//
// The trigger that brought this row up: the APK genuinely does not ship
// AudioPluginOculusSpatializer, so every
// world load raises DllNotFoundException, and the managed response is:
//
//     E/Unity: Exception: Going to Error World
//
// The route to the code, since nothing here has a symbol:
// tools/vrc_code.py --literals 'Going to Error World' finds
// StringLiteral[13074]; --uses-literal names its one real reader,
// VRCFlowManager::ÌÌÎÌÍÌÌÍÌÏÍÍÌÌÌÍÎÍÏÍÏÎÌ at 0x6944074 (196 bytes). That
// method IS the error-world action, not a branch near it: it builds a
// System.Exception with that message, hands it to the shared LogException
// helper at 0x68acbd0 (the VRC.Core.Logger user — this is what prints the
// E/Unity line above), then starts the coroutine stored in [this + 0x148]
// through the generic coroutine starter at 0x6940030, and returns the
// Coroutine to its caller, which yields on it. The starter has nine other
// callers and is innocent; the literal's only other reader, a <>c lambda at
// 0x6aed654, just string.Formats the message. So the whole transition is
// exactly this one small method.
//
// --callers shows all six call sites live in two coroutines' MoveNext and
// every one is a failure path ('Failed to load default world {0} times,
// going to error world', "Couldn't validate world {0}", and their untagged
// siblings): the method is never reached from a healthy flow, so making it
// a no-op disables ONLY the error-driven transition. What the host loses is
// the Error World itself — a fallback that exists to give the user somewhere
// to stand when the real world failed, which is precisely the thrash when
// the failure is this host's missing spatializer. The DllNotFoundException
// is still logged by Unity; only the reaction is skipped.
//
// The replacement mirrors the wall row above: return before the prologue,
// with x0 = null so the caller's `yield return <coroutine>` degenerates to
// the benign `yield return null` (verified in the caller at 0x69701c0: the
// returned pair is stored, never dereferenced, before MoveNext continues).
//
//   0x6944074  sub sp, sp, #0x30        -> mov x0, xzr  ; "no transition"
//   0x6944078  stp x30, x21, [sp, #0x10] -> ret
static const kl_gp_word k_vrc_errorworld[] = {
    { 0x6944074, 0xd100c3ff, 0xaa1f03e0, "mov x0, xzr  (return null coroutine)" },
    { 0x6944078, 0xa90157fe, 0xd65f03c0, "ret" },
};

// (4) The stereo rendering mode, in libUnityOpenXR.so rather than in the
// managed image — the one row here that is about pixels.
//
// This build asks for a swapchain with `arraySize 2`, which is the Single Pass
// Instanced layout, and single-pass instanced on GLES is `GL_OVR_multiview2`.
// ANGLE's Metal backend has no multiview at all (nothing in
// src/libANGLE/renderer/metal mentions it), so we advertise none, libunity
// resolves no multiview entry point, and its fall-back attaches ONE array
// layer — layer 0 — and never re-attaches. The right eye is therefore never
// drawn: measured as tex 29 layer 0 at 3,493,465 lit and layer 1 at 0 lit, in
// every frame of every run, with no GL error anywhere.
//
// The mode is not a question asked of this host. Unity's OpenXR C# pushes it
// down once, from the serialized `OpenXRSettings.renderMode`, through
// `NativeConfig_SetRenderMode(int)` — which is the whole of the decision:
//
//   0x76e08  stp x19, x30, [sp, #-0x10]!
//   0x76e0c  mov w19, w0                 -> mov w19, #0     ; MultiPass
//   0x76e10  bl  <singleton>
//   0x76e14  cbz x0, +0x18
//   0x76e18  bl  <singleton>
//   0x76e1c  str w19, [x0, #0x70]        ; the field every consumer reads
//
// `MultiPass = 0, SinglePassInstanced = 1` is the package's own enum, and the
// binary agrees: the consumer at +0x55120 tests the field against exactly 0 and
// 1 and treats anything else as neither.
//
// MultiPass is the mode that needs no extension: two passes, one array slice
// each, the plain (non-stereo) shader variant that every build ships. It costs
// twice the draw calls, which is the honest price of a display without
// multiview. `KL_GUEST_PATCH_OFF=vrchat-multipass` restores the failing
// configuration exactly.
static const kl_gp_word k_vrc_multipass[] = {
    { 0x76e0c, 0x2a0003f3, 0x52800013, "mov w19, w0 -> mov w19, #0  (MultiPass)" },
};


// (5) Unity's cross-context guard on every framebuffer bind.
//
// Framebuffer objects are container objects and are NOT shared between GL
// contexts, so Unity records the EGLContext each binding was made under and
// re-checks it before every bind. On a mismatch it binds `(GLuint)-1` rather
// than a name it believes is invalid here. The idiom is inlined at 35 sites,
// always the same three instructions, always calling GL table slot 1240:
//
//     cmp   x8, x20                 ; recorded context vs the live one
//     ccmp  wN, #0, #4, ne          ; ...or the name is 0
//     csinv w1, wN, wzr, eq         ; mismatch -> ~0
//
// `(GLuint)-1` is not a sentinel ANGLE invents and not a refused bind: the
// guest passes it, ANGLE accepts it, and the result is a framebuffer with no
// attachments — so the clear and every draw of that pass raise
// GL_INVALID_FRAMEBUFFER_OPERATION and go nowhere. Measured on VRChat: 322 such
// errors in a 1500-frame run, 0 with this row applied.
//
// Neutralising the guard is truthful about what this runtime implements, and
// that is the whole justification: kl_glfb's default migration mode backs EVERY
// guest EGLContext with ONE real ANGLE context (see kl_glfb_make_current), so
// the framebuffer really is valid whichever guest context is nominally current.
// The distinction Unity is guarding against does not exist here.
//
// Three things about this row that must not be forgotten:
//
//   * WHAT TRIGGERS THE MISMATCH IS NOT EXPLAINED. Every eglGetCurrentContext
//     in the run answers the same context, and every framebuffer is created
//     under it, so the recorded value must be captured somewhere this
//     instrumentation did not see. Beat Saber never binds -1 at all; AVPro's
//     third context was suspected and MEASURED INNOCENT (refusing it leaves two
//     contexts and the -1 binds remain).
//   * IT DOES NOT FIX THE WORLD LOAD. VRChat still fails to instantiate a
//     scene with this applied — that is a separate cause, and the adjacency of
//     the two in the log is a coincidence.
//   * On device it is applied OFFLINE by tools/klepton_ld.c, like every row
//     here, so the host and the headset agree.
static const kl_gp_word k_vrc_fbo_ctx[] = {
    { 0x1432f04, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458650, 0x5a9f02a1, 0x2a1503e1, "w1 = w21 unconditionally" },
    { 0x145888c, 0x5a9f03a1, 0x2a1d03e1, "w1 = w29 unconditionally" },
    { 0x145899c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458a54, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458ac4, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x1458b34, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x1458bdc, 0x5a9f0281, 0x2a1403e1, "w1 = w20 unconditionally" },
    { 0x1458c70, 0x5a9f02a1, 0x2a1503e1, "w1 = w21 unconditionally" },
    { 0x14590a4, 0x5a9f0141, 0x2a0a03e1, "w1 = w10 unconditionally" },
    { 0x1459240, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x145beb8, 0x5a9f0101, 0x2a0803e1, "w1 = w8 unconditionally" },
    { 0x14688dc, 0x5a9f0141, 0x2a0a03e1, "w1 = w10 unconditionally" },
    { 0x1468cdc, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x1469ff8, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146a2e0, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x146aff0, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146b3bc, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146bc68, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c10c, 0x5a9f0341, 0x2a1a03e1, "w1 = w26 unconditionally" },
    { 0x146c190, 0x5a9f0341, 0x2a1a03e1, "w1 = w26 unconditionally" },
    { 0x146c24c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c2b0, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146c328, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c51c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c84c, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146c94c, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146ca9c, 0x5a9f0161, 0x2a0b03e1, "w1 = w11 unconditionally" },
    { 0x146cc24, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146ccd8, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x146cd4c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146d260, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146d61c, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146d6bc, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x146d7ec, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
};


// (6) The AudioPluginOculusSpatializer per-frame exception storm.
//
// The Steam Frame APK ships the Oculus spatializer's MANAGED code but not its
// native module, so every P/Invoke into it throws DllNotFoundException — on
// real hardware too, not just here. Measured: ~1 throw per frame, every frame
// (587-609 in 600-frame headless runs), and each one costs a full IL2CPP
// exception raise (our dlopen miss is cached; the throw is not). The call
// site evaded four static hunts because the
// module name lives only in P/Invoke metadata and the resolver is reached
// indirectly.
//
// The route that worked: `vrc_code.py --type '^OculusSpatializerUnity$'`
// lists the component's 27 methods; the small ones (124-136 bytes, no
// metadata usages) are IL2CPP's P/Invoke thunks, and each embeds its
// resolver data in plain sight — an adrp+add pair naming the module string
// ('AudioPluginOculusSpatializer', 28 chars) and another naming the native
// entry point. Reading those strings out of the binary attributes every
// thunk: OSP_Unity_AssignRaycastCallback (two variants), SetDynamicRoom*,
// UpdateRoomModel, GetRoomDimensions, GetRaycastHits.
//
// The per-frame caller is OculusSpatializerUnity::Update (0x67a0c80). Its
// prologue reads a bool at [this+0x38]: if set it builds a delegate and
// calls the marshaling AssignRaycastCallback variant (0x679dab4) at
// 0x67a0d88; either way the paths merge at 0x67a0e50 and call the plain
// AssignRaycastCallback thunk (0x679f1e0) at 0x67a0e68, followed by the
// SetDynamicRoom* chain. The FIRST of these to run throws, and exactly one
// throw per frame says the exception unwinds clean out of Update — so on
// real hardware everything in Update after that call is dead code, every
// frame, forever.
//
// The patch therefore does by branch what hardware does by exception: at
// each of the two throwing call sites, jump to Update's own epilogue
// (0x67a13e8, the register-restore ladder ending in ret at 0x67a1414). No
// managed side effect is skipped that hardware would have performed — on
// hardware the throw unwinds past the same instructions. The spatializer's
// dynamic-room feature is off either way; what changes is only the cost and
// the log. One-time throws from Start (same thunks, other call sites) are
// left alone: they are once, not per frame.
//
//   0x67a0d88  bl 0x679dab4  -> b 0x67a13e8   ; [this+0x38] path
//   0x67a0e68  bl 0x679f1e0  -> b 0x67a13e8   ; merged per-frame path
//
// Measured: objdump + a raw PT_LOAD read agree on both expect words; the
// epilogue at 0x67a13e8 begins `ldp x20, x19, [sp, #0xa0]` (0xa94a4ff4),
// matching the prologue's stores at 0x67a0ca0.
static const kl_gp_word k_vrc_ospstorm[] = {
    { 0x67a0d88, 0x97fff34b, 0x14000198, "bl AssignRaycastCallback(marshaled) -> b Update.epilogue" },
    { 0x67a0e68, 0x97fff8de, 0x14000160, "bl AssignRaycastCallback -> b Update.epilogue" },
};

static const kl_gp_patch k_patches[] = {
    { "vrchat-multipass", "libUnityOpenXR.so",
      "the stereo rendering mode is MultiPass, because there is no multiview",
      k_vrc_multipass, sizeof k_vrc_multipass / sizeof *k_vrc_multipass },
    { "vrchat-under-construction", "libil2cpp.so",
      "the VRCFlowManager wall coroutine completes instead of hanging",
      k_vrc_wall, sizeof k_vrc_wall / sizeof *k_vrc_wall },
    { "vrchat-fbo-context-guard", "libunity.so",
      "EXPERIMENT: framebuffer binds ignore Unity's cross-context guard",
      k_vrc_fbo_ctx, sizeof k_vrc_fbo_ctx / sizeof *k_vrc_fbo_ctx },
    { "vrchat-min-client-version", "libil2cpp.so",
      "the client build number always compares new enough",
      k_vrc_minver, sizeof k_vrc_minver / sizeof *k_vrc_minver },
    { "vrchat-error-world", "libil2cpp.so",
      "the exception-driven Error World transition does not start",
      k_vrc_errorworld, sizeof k_vrc_errorworld / sizeof *k_vrc_errorworld },
    { "vrchat-spatializer-storm", "libil2cpp.so",
      "the absent Oculus spatializer is skipped, not thrown on, per frame",
      k_vrc_ospstorm, sizeof k_vrc_ospstorm / sizeof *k_vrc_ospstorm },
};
#define KL_GP_N (sizeof k_patches / sizeof k_patches[0])

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Is `name` listed in KL_GUEST_PATCH_OFF? Compared whole, against a comma or
// space separated list, so one name is never a prefix of another's answer.
static int turned_off(const char *name) {
    const char *list = getenv("KL_GUEST_PATCH_OFF");
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) return 1;
        p = e;
    }
    return 0;
}

unsigned kl_guest_patch_apply(const char *path, kl_gp_at at, void *ctx) {
    if (!kl_env_on("KL_GUEST_PATCH", 1)) return 0;
    const char *base = basename_of(path);
    unsigned applied = 0;

    for (size_t i = 0; i < KL_GP_N; i++) {
        const kl_gp_patch *p = &k_patches[i];
        if (strcmp(base, p->lib) != 0) continue;
        if (turned_off(p->name)) {
            fprintf(stderr, "  [patch] %s: OFF (KL_GUEST_PATCH_OFF)\n", p->name);
            continue;
        }

        // Pass 1 — every word must be where it was measured. The two ways that
        // fails are NOT the same thing and are reported differently, which the
        // first run of this table got wrong: every guest with a `libil2cpp.so`
        // matches the basename, so Beat Saber's `make check` printed two lines
        // about VRChat's rows. An address past the end of the image says this
        // is a different LIBRARY that happens to share a name — no information,
        // so it is silent unless asked. A word that is present and different
        // says this is the right library and the wrong BUILD, which is worth a
        // line: it is the difference between "not for this APK" and "wrong".
        unsigned ok = 1;
        for (unsigned k = 0; k < p->nwords && ok; k++) {
            uint32_t *w = at(ctx, p->words[k].va);
            if (!w) {
                if (kl_env_on("KL_TRACE_PATCH", 0))
                    fprintf(stderr, "  [patch] %s: not this image — 0x%llx is "
                                    "past the end of %s\n",
                            p->name, (unsigned long long)p->words[k].va, base);
                ok = 0;
            } else if (*w != p->words[k].expect) {
                fprintf(stderr, "  [patch] %s: SKIPPED — 0x%llx holds 0x%08x, "
                                "measured 0x%08x. This is not the build it was "
                                "measured against; the image is untouched.\n",
                        p->name, (unsigned long long)p->words[k].va,
                        *w, p->words[k].expect);
                ok = 0;
            }
        }
        if (!ok) continue;

        // Pass 2 — write, now that the whole group is known to fit.
        for (unsigned k = 0; k < p->nwords; k++)
            *at(ctx, p->words[k].va) = p->words[k].to;
        applied++;
        fprintf(stderr, "  [patch] %s: applied %u word(s) to %s — %s\n",
                p->name, p->nwords, base, p->why);
        if (kl_env_on("KL_TRACE_PATCH", 0))
            for (unsigned k = 0; k < p->nwords; k++)
                fprintf(stderr, "          0x%09llx 0x%08x -> 0x%08x  %s\n",
                        (unsigned long long)p->words[k].va, p->words[k].expect,
                        p->words[k].to, p->words[k].what);
    }
    return applied;
}
