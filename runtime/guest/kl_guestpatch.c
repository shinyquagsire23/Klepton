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

// ---------------------------------------------------------------------------
// Liminal (`LiminalSdk.dll` inside libil2cpp.so, Unity 2019.1.10f1 / Oculus
// Utilities 1.40).
//
// GearVRDevice.UpdateConnectedControllers decides whether to build the Touch
// laser/reticle from OVRInput.IsControllerConnected, and that check reads
// OVRInput's connectedControllers cache. It runs ONCE at startup (frame ~800),
// BEFORE OVRManager is added and OVRInput first polls (frame ~1800), so the
// cache is empty and both hands read as disconnected — the app commits to its
// gaze profile and never re-checks, leaving no pointer to click with. The
// controllers ARE present the whole time (our GetControllerState4 reports Touch
// connected every poll); the only lie is the momentary empty cache.
//
// IsControllerConnected is (connectedControllers & mask) == mask, at 0x2dcdb14:
//   0x2dcdb6c  ldr x8, [x0, #184]     ; the OVRInput state object
//   0x2dcdb74  ldr w8, [x8, #20]      ; connectedControllers  <-- patched
//   0x2dcdb78  bics wzr, w19, w8      ; mask & ~connected
//   0x2dcdb7c  cset w0, eq            ; == mask ?
// Replacing the load with `orr w8, wzr, #3` makes the check see LTouch|RTouch
// as always connected and nothing else — true for the PSVR2-mapped Touch
// controllers, and it leaves every non-Touch query (Go remotes) answering
// false. Pair with KL_OVRP_HEADSET=8 so the hand->controller-type mapping
// resolves to Touch rather than the 3DoF remote (see klovrp_headset_type).
static const kl_gp_word k_liminal_touch[] = {
    { 0x2dcdb74, 0xb9401508, 0x320007e8, "orr w8, wzr, #3  (Touch always connected)" },
};

// ---------------------------------------------------------------------------
// Wrath2 (libUE4.so) — force the Vulkan-RHI gate open.
//
// FAndroidMisc::IsVulkanAvailable() (@0xc319344, confirmed by disassembly)
// returns false, so UE4 puts up "This device does not support Vulkan but the app
// was not packaged with ES 3.1 support" and quits (SIGKILL, no device ever
// created). Its logic requires, among other things,
// (bSupportsVulkan==1 || bSupportsVulkanSM5==1) read from the cooked
// [/Script/AndroidRuntimeSettings.AndroidRuntimeSettings] section — both preset
// FALSE — and ModuleExists("VulkanRHI"). A command-line
// "-ini:…:bSupportsVulkan=True" override was applied (verified in the log) and
// did NOT flip the verdict, so the failing gate is either that the -ini is read
// after this early check or ModuleExists is false. Rather than keep guessing,
// force the function itself: overwrite its prologue with `mov w0, #1; ret` so it
// always answers "available". If wrath2 then proceeds into real Vulkan init
// (vkCreateDevice / swapchain / shader compile), the gate WAS the only blocker;
// if it fails past this, the next wall is named honestly. The `expect` words are
// this build's prologue, so no other libUE4 title is touched. KL_GUEST_PATCH_OFF=
// wrath2-force-vulkan-available disables it.
//   0xc319344  sub sp, sp, #0x60          -> mov w0, #1
//   0xc319348  stp x24, x23, [sp, #0x20]  -> ret
static const kl_gp_word k_wrath2_forcevk[] = {
    { 0xc319344, 0xd10183ff, 0x52800020, "mov w0, #1  (IsVulkanAvailable -> true)" },
    { 0xc319348, 0xa9025ff8, 0xd65f03c0, "ret" },
};

// Wrath2 (libUE4.so) — force the "COMPILING SHADERS" title segment to complete.
//
// AAW_CompileShadersTitleScreenSegment gates on
// FShaderPipelineCache::NumPrecompilesRemaining() reaching 0, but the precompile
// stalls (~529 PSOs then never drains — the batched precompile stops making
// progress), so the segment loops forever and never advances to Calibration.
// Force NumPrecompilesRemaining() to return 0 so the segment finishes; the cache
// still opens normally and PSOs compile on demand (first-use hitches, not a hang).
//   0xcded830  str d8, [sp, #-0x50]!      -> mov w0, #0
//   0xcded834  stp x24, x23, [sp, #0x10]  -> ret
static const kl_gp_word k_wrath2_pso_done[] = {
    { 0xcded830, 0xfc1b0fe8, 0x52800000, "mov w0, #0  (NumPrecompilesRemaining -> 0)" },
    { 0xcded834, 0xa9015ff8, 0xd65f03c0, "ret" },
};

// NOTE (hl2/libtogl.so multiview): TWO reverted experiments now.
// (1) Forcing the generate BIT on (0xa1788 `tbnz w23,#0` -> `b`) fired generation at
//     precache and poisoned the "already tried" latch (vshader[0x64]=1).
// (2) NOP the latch short-circuit at 0xa0878 so a poisoned shader retries — REVERTED.
//     Confirmed on-device the retry DOES happen (each "no usable variant" now logs
//     twice) but the generator (0xa0808) STILL returns 0 every time, AND it regressed
//     boot (never reached the black screen). So the failure is NOT the latch:
//     - the port IS requesting multiview variants at draw time (it logs "no usable
//       variant for lightmappedgeneric_vs20/skin_vs20/..."), so the generate bit is set;
//     - the marker `gl_Position = vTempPos;` IS present in the translated GLSL (dumped
//       in the log), and the 0xa0aac "unsupported vertex shader layout" warning does
//       NOT fire, so the generator passes the marker check;
//     - there is NO ANGLE compile error logged either.
//     => the generator fails DEEPER in its body (0xa0918+, past the marker), returning
//     0 for a reason not yet traced. That is the real thing to chase — read the full
//     generator body's return-0 paths. libtogl is per-title (portal's is a different
//     binary), so a future fix can be fingerprint-keyed to hl2 safely.

// hl2 multiview shader-gen: three patch experiments tried, all applied (mkguest.sh
// re-translates every build; --quiet just hides klepton-ld's "[patch] ... applied"),
// none fixed it: (1) 0xa1788 tbnz->b force; (2) 0xa0878 latch NOP; (3) 0xa1a48
// `ldr w3` -> `mov w3,#1`. With (3) the generator STILL never reaches glCompileShader
// (num_views/gl_ViewID_OVR count = 0 in the log) and no "compile failed"/"unsupported
// layout" warning fires — so the generator (0xa0808) returns 0 at a SILENT path
// (a082c [0x8]!=0, a0868 [0x18]==0, or a0890 [0x4c]==0), i.e. before it builds/compiles.
// Likely the world shaders route through the OTHER caller 0xa1eec (w3=w20, from the
// enclosing fn's w3 arg), which (3) did not force; or the shader object handed to the
// generator has no translated GLSL ([0x18]==0) at that moment (an ordering issue).
// NEXT: force w23 INSIDE the variant-select (a172c `mov w23,w3` -> `mov w23,#1`) to
// cover BOTH callers, AND if num_views still 0, add a runtime probe on the generator's
// silent return-0 paths (the guest generator can't be logged, so instrument via a
// klepton-side hook on glCreateShader/glShaderSource to see if a multiview source ever
// arrives). libtogl is per-title (portal's differs), so any force is fingerprint-safe.

// Force w23=1 INSIDE the variant-select (0xa1704), covering BOTH callers (a1a48 and
// a1eec) at once — the single-caller a1a48 force left num_views at 0, so the world
// shaders likely route through a1eec. If the generator now reaches glCompileShader
// (num_views appears) we learn whether ANGLE accepts the multiview GLSL; if it stays
// 0, the generator fails at a SILENT pre-build gate ([0x18]==0 GLSL-not-ready) and the
// fix is a timing/ordering one, not a force. Fingerprint-keyed (portal's a172c is
// `mov x0,x19`).
__attribute__((unused))  // disabled in k_patches[] (two-pass supersedes it); kept for Strategy-A revert
static const kl_gp_word k_hl2_mv_force2[] = {
    { 0xa172c, 0x2a0303f7, 0x52800037,
      "mov w23, #1  (force multiview generate bit for BOTH variant-select callers)" },
};

// hl2 (HL2Q3VR): the mod gates VR eye render-target creation on "Queued Material System"
// being ENABLED (HL2Q3 MatQueue allow=1). That decision (libmaterialsystem 0xe4998) computes
// allow = (CommandLine("-threads",2) > 1) & arg1, but only after a CPUInfo[5] >= 2 gate; under
// Klepton it lands allow=0 (ideal stays 0), so CreateRenderTargets (libsourcevr 0xec64) is
// never called -> "Eye copy skipped: Source render target is unavailable" -> black (see
// hl2-quest-reference-trace). Quest reaches allow=1 -> RTs -> renders. Force it: NOP the
// CPUInfo<2 disable branch, and make the allow store unconditional 1. EXPERIMENT — this
// enables the mod's queued (threaded-GL) path; on Quest it enables only to build the RTs then
// forces off, so this may open the RT-creation window. If ANGLE-Metal can't do threaded GL it
// may fault instead — that itself is the answer.
__attribute__((unused))  // disabled in k_patches[] — confirmed a tangent (see registration)
static const kl_gp_word k_hl2_matqueue_allow[] = {
    { 0xe49f4, 0x54000223, 0xd503201f, "NOP the CPUInfo<2 'disable queued' branch" },
    { 0xe49f8, 0x0a150308, 0x320003e8, "and w8,w24,w21 -> mov w8,#1  (allow=1 forced)" },
};

static const kl_gp_patch k_patches[] = {
    // DISABLED: "hl2-multiview-force2" forced libtogl to generate a MULTIVIEW shader
    // variant for every world shader — Strategy A (make ANGLE-Metal multiview work),
    // which never got past "no usable vertex shader variant". We now commit to
    // Strategy B: two-pass per-eye rendering (portal's working path) via the
    // synthesized autoexec.cfg (hl2quest_vr_single_pass_* 0, kl_libc.c). Two-pass
    // draws each eye with NORMAL shaders, so forcing multiview variants in libtogl
    // actively BREAKS it (the engine asks for a normal shader, the patch makes a
    // multiview one, mismatch -> black). Re-enable only if reverting to Strategy A;
    // the two are mutually exclusive. (void)k_hl2_mv_force2 keeps it referenced.
    // { "hl2-multiview-force2", "libtogl.so",
    //   "force the multiview generate bit inside the variant-select (both callers)",
    //   k_hl2_mv_force2, sizeof k_hl2_mv_force2 / sizeof *k_hl2_mv_force2 },
    // DISABLED: confirmed a TANGENT. The Quest boot log shows CreateRenderTargets runs with
    // "Material queued rendering allowed: mode 0 -> 0 (previouslyAllowed=0)" — i.e. it creates
    // the eye RTs BEFORE the MatQueue ever enables queued mode, so MatQueue allow is IRRELEVANT
    // to RT creation. This patch correctly made the MatQueue match Quest (allow=1/ideal=2) but
    // did NOT trigger CreateRenderTargets, and enabling queued (threaded-GL) mode under
    // ANGLE-Metal is unnecessary risk. Kept for reference. (void) keeps it referenced.
    // { "hl2-matqueue-allow", "libmaterialsystem.so",
    //   "force HL2Q3 Queued Material System allow=1 so VR render targets get created",
    //   k_hl2_matqueue_allow, sizeof k_hl2_matqueue_allow / sizeof *k_hl2_matqueue_allow },

    { "wrath2-force-vulkan-available", "libUE4.so",
      "FAndroidMisc::IsVulkanAvailable() always returns true (the RHI gate)",
      k_wrath2_forcevk, sizeof k_wrath2_forcevk / sizeof *k_wrath2_forcevk },
    { "wrath2-pso-done", "libUE4.so",
      "FShaderPipelineCache::NumPrecompilesRemaining() returns 0 (title segment completes)",
      k_wrath2_pso_done, sizeof k_wrath2_pso_done / sizeof *k_wrath2_pso_done },

    { "liminal-touch-connected", "libil2cpp.so",
      "the Touch controllers read as connected at the startup device check",
      k_liminal_touch, sizeof k_liminal_touch / sizeof *k_liminal_touch },

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

// ---------------------------------------------------------------------------
// unity-texunit-mask: FOUND, not measured — the one row in this file that is.
//
// Unity's GLES backend caps texture units at min(GL_MAX_COMBINED_TEXTURE_IMAGE_
// UNITS, 32) and sizes its GL state cache to that (bound name and target per
// unit, 32 words each), while HLSLCC bakes sampler units ABOVE 31 into shaders
// that carry many uniforms: a sampler's unit is its explicit location, and the
// samplers come after everything else. Unity refuses those binds ("OpenGL
// Error: Invalid texture unit!") and the sampler reads whatever unit 0 left
// behind: black surfaces, tofu text. Raising the cap (kl_guestpoke.c) lets the
// binds through and overruns the cache — Iron Rebellion bound draw framebuffer
// 0xffffffff for it. Measured on six titles from 2019.1 to 2022.3.
//
// Every program's sampler units are ONE contiguous block (they are its last
// locations), so `unit & 31` is collision-free inside any program. Applied at
// both ends it keeps every unit under 32: here, in the function that owns the
// reject — `mov wU, wArg` -> `and wU, wArg, #31` on its way in, before the cap
// check and every cache write — and in kl_glfb's glUniform1i for sampler-typed
// uniforms, which asks kl_gp_texunit_state() whether THIS image carries it.
//
// The same shape in every build measured (the rows in kl_guestpoke.c): the
// error string, one adrp+add that loads it, a `cmp wCap, wU` / `b.ls` into
// that block, and a `mov wU, w<arg>` earlier in the function. Anything else is
// refused by name and the image is left alone. KL_GUEST_PATCH_OFF=
// unity-texunit-mask turns it off.

static const char k_texunit_msg[] = "OpenGL Error: Invalid texture unit!";

static int gp_byte(kl_gp_at at, void *ctx, uint64_t va, uint8_t *out) {
    uint32_t *w = at(ctx, va & ~3ull);
    if (!w) return 0;
    *out = (uint8_t)(*w >> (8 * (va & 3)));
    return 1;
}

// The guard, located: where the unit register is defined, and what is there
// now. 0 = not this shape (or no string), 1 = the original `mov`, 2 = already
// masked. *def_va and *orig receive the site and the word to restore it to.
static int texunit_find(kl_gp_at at, void *ctx, uint64_t *def_va, uint32_t *mov_word,
                        uint32_t *and_word, const char **why) {
    // 1. the string. It sits in the first LOAD with the code, which starts at 0.
    const size_t n = sizeof k_texunit_msg;           // with the NUL: a whole string
    uint64_t str = 0; int found = 0;
    for (uint64_t va = 0; at(ctx, va & ~3ull); va++) {
        size_t k = 0; uint8_t b;
        while (k < n && gp_byte(at, ctx, va + k, &b) && b == (uint8_t)k_texunit_msg[k]) k++;
        if (k == n) { str = va; found = 1; break; }
    }
    if (!found) { *why = "no \"Invalid texture unit\" string"; return 0; }

    // 2. its one reference: add xD, xS, #lo12 with adrp xS, page a few words back.
    uint64_t ref = 0;
    for (uint64_t va = 0; at(ctx, va); va += 4) {
        uint32_t w = *at(ctx, va);
        if ((w & 0xFFC00000u) != 0x91000000u) continue;              // add x, x, #imm
        if (((w >> 10) & 0xfff) != (str & 0xfff)) continue;
        unsigned rn = (w >> 5) & 31;
        for (int back = 1; back <= 8 && va >= (uint64_t)back * 4; back++) {
            uint32_t *pw = at(ctx, va - back * 4);
            if (!pw || (*pw & 0x9F000000u) != 0x90000000u || (*pw & 31) != rn) continue;
            int64_t imm = (int64_t)((((*pw >> 5) & 0x7FFFF) << 2) | ((*pw >> 29) & 3));
            if (imm & (1ll << 20)) imm -= (1ll << 21);
            if ((((va - back * 4) & ~0xfffull) + (uint64_t)(imm << 12)) == (str & ~0xfffull)) {
                ref = va; break;
            }
        }
        if (ref) break;
    }
    if (!ref) { *why = "no adrp+add of the string"; return 0; }

    // 3. the guard: a b.ls landing just before the reference, and the cmp before it.
    uint64_t cmp_va = 0; unsigned unit = 32;
    for (uint64_t va = ref; va >= 4 && ref - va < 4096; va -= 4) {
        uint32_t *pw = at(ctx, va);
        if (!pw) break;
        uint32_t w = *pw;
        if ((w & 0xFF000010u) != 0x54000000u || (w & 0xf) != 0x9) continue;   // b.ls
        int64_t off = (int64_t)((w >> 5) & 0x7FFFF);
        if (off & (1ll << 18)) off -= (1ll << 19);
        uint64_t tgt = va + (uint64_t)(off * 4);
        if (tgt > ref || ref - tgt > 64) continue;
        uint32_t *pc = at(ctx, va - 4);
        if (!pc || (*pc & 0xFFE0FC1Fu) != 0x6B00001Fu) continue;         // cmp wN, wM
        cmp_va = va - 4; unit = (*pc >> 16) & 31;
        break;
    }
    if (!cmp_va) { *why = "no cmp/b.ls guard into the reject"; return 0; }

    // 4. where the unit register was set. Nothing between there and the cmp
    //    may name it as a destination (low five bits) — conservative, so a
    //    store through it refuses too, which is the safe direction.
    for (uint64_t va = cmp_va - 4; cmp_va - va <= 64 * 4; va -= 4) {
        uint32_t *pw = at(ctx, va);
        if (!pw) break;
        uint32_t w = *pw;
        if (w == 0xd65f03c0u) break;                                       // ret: left the function
        if ((w & 0xFFE0FFE0u) == 0x2A0003E0u && (w & 31) == unit) {      // mov wU, wM
            unsigned src = (w >> 16) & 31;
            if (src > 7) { *why = "the unit is not moved from an argument"; return 0; }
            *def_va = va; *mov_word = w;
            *and_word = 0x12001000u | (src << 5) | unit;                  // and wU, wM, #31
            return 1;
        }
        if ((w & 0xFFFFFC00u) == 0x12001000u && (w & 31) == unit) {      // and wU, wM, #31
            unsigned src = (w >> 5) & 31;
            *def_va = va; *and_word = w;
            *mov_word = 0x2A0003E0u | (src << 16) | unit;
            return 2;
        }
        if ((w & 31) == unit) { *why = "the unit register is written between its "
                                       "definition and the cap check"; return 0; }
        if (va < 4) break;
    }
    *why = "no `mov wU, w<arg>` before the cap check";
    return 0;
}

int kl_gp_texunit_state(kl_gp_at at, void *ctx) {
    uint64_t def; uint32_t mov, and_; const char *why = NULL;
    return texunit_find(at, ctx, &def, &mov, &and_, &why);
}

static unsigned texunit_apply(const char *base, kl_gp_at at, void *ctx) {
    if (turned_off("unity-texunit-mask")) {
        fprintf(stderr, "  [patch] unity-texunit-mask: OFF (KL_GUEST_PATCH_OFF)\n");
        return 0;
    }
    uint64_t def = 0; uint32_t mov = 0, and_ = 0; const char *why = "?";
    int st = texunit_find(at, ctx, &def, &mov, &and_, &why);
    if (st == 0) {
        fprintf(stderr, "  [patch] unity-texunit-mask: SKIPPED — %s in %s; the image "
                        "is untouched\n", why, base);
        return 0;
    }
    if (st == 2) return 0;                         // a second pass over one image
    *at(ctx, def) = and_;
    fprintf(stderr, "  [patch] unity-texunit-mask: applied to %s — texture units are "
                    "taken & 31 at the bind (0x%llx: 0x%08x -> 0x%08x)\n",
            base, (unsigned long long)def, mov, and_);
    return 1;
}

// ---------------------------------------------------------------------------
// unity-no-compute: FOUND, like unity-texunit-mask above.
//
// Unity's GLES device marks compute as supported from the GL VERSION alone: at
// ES 3.1+ it sets bit 0x4000 (compute) in the shader-requirement mask it keeps
// at caps+0xd0, unconditionally, and only then reads the storage and compute
// limits — so no answer we give to those queries can take it back, and
// describing ES 3.0 instead costs the post-processing stack (kl_egl.c). But
// ANGLE here is ES 3.0 and has no compute stage at all: glCreateShader(
// COMPUTE) is INVALID_ENUM, every compute shader fails ("GLSL compilation
// failed, no infolog"), and GPU skinning with it ("Internal-Skinning.compute:
// Kernel … invalid") — so every skinned mesh is simply not drawn. Iron
// Rebellion's enemy mechs were invisible for exactly this (349k errors a run).
//
// The `orr wN|xN, …, #0x4000` that sets the bit sits beside the first limit
// query, MAX_SHADER_STORAGE_BUFFER_BINDINGS (`mov w0, #0x90dd`), in every build
// measured (2019.2.18 .. 2021.3.9), followed by its store into the mask. It
// becomes a nop: Unity then reports no compute, skins on the CPU, and takes a
// shader's fallback SubShader instead of one that needs a compute stage.
//
// A title whose required shaders need compute reported would lose them to
// this; KL_GUEST_PATCH_OFF=unity-no-compute turns it off for such a game.

static int nocompute_find(kl_gp_at at, void *ctx, uint64_t *site, uint32_t *word,
                          const char **why) {
    const uint32_t k_mov_ssbo = 0x52800000u | (0x90ddu << 5);   // mov w0, #0x90dd
    uint64_t q = 0; unsigned nq = 0;
    for (uint64_t va = 0; at(ctx, va); va += 4)
        if (*at(ctx, va) == k_mov_ssbo) { if (!nq) q = va; nq++; }
    if (nq != 1) { *why = nq ? "the storage-buffer query appears more than once"
                             : "no storage-buffer query (mov w0, #0x90dd)"; return 0; }
    for (int d = -16; d <= 8; d++) {
        if (d < 0 && q < (uint64_t)(-d) * 4) continue;
        uint64_t va = q + (int64_t)d * 4;
        uint32_t *pw = at(ctx, va);
        if (!pw) continue;
        uint32_t w = *pw;
        unsigned rd = w & 31;
        uint32_t want32 = 0x32120000u | (rd << 5) | rd;              // orr wN, wN, #0x4000
        uint32_t want64 = 0xB2720000u | (rd << 5) | rd;              // orr xN, xN, #0x4000
        if (w != want32 && w != want64 && w != 0xD503201Fu) continue;
        if (w == 0xD503201Fu) continue;                              // a nop proves nothing
        for (int s = 1; s <= 3; s++) {                               // ...and its store
            uint32_t *ps = at(ctx, va + s * 4);
            if (!ps) break;
            uint32_t sw = *ps;
            int is_str = (w == want32) ? ((sw & 0xFFC00000u) == 0xB9000000u)
                                       : ((sw & 0xFFC00000u) == 0xF9000000u);
            if (is_str && (sw & 31) == rd) { *site = va; *word = w; return 1; }
        }
    }
    // Already patched? The mask's store with no orr before it, and a nop where
    // one was, is what a second pass over one image looks like.
    *why = "no `orr …, #0x4000` and store beside the storage-buffer query";
    return 0;
}

static unsigned nocompute_apply(const char *base, kl_gp_at at, void *ctx) {
    if (turned_off("unity-no-compute")) {
        fprintf(stderr, "  [patch] unity-no-compute: OFF (KL_GUEST_PATCH_OFF)\n");
        return 0;
    }
    uint64_t site = 0; uint32_t w = 0; const char *why = "?";
    if (!nocompute_find(at, ctx, &site, &w, &why)) {
        fprintf(stderr, "  [patch] unity-no-compute: SKIPPED — %s in %s; the image is "
                        "untouched\n", why, base);
        return 0;
    }
    *at(ctx, site) = 0xD503201Fu;
    fprintf(stderr, "  [patch] unity-no-compute: applied to %s — compute is not reported, "
                    "so skinning runs on the CPU (0x%llx: 0x%08x -> nop)\n",
            base, (unsigned long long)site, w);
    return 1;
}

// ---------------------------------------------------------------------------
// unity-cpu-skinning: FOUND, for the 2021.3 and 2019.4 shapes.
//
// unity-no-compute clears SystemInfo.supportsComputeShaders, but that is NOT
// what decides skinning. IsGPUSkinningEnabled() (Iron Rebellion's libunity
// 0x76e614) is PlayerSettings.gpuSkinning && (caps+0x414 & 0x01); with it on,
// every skinned mesh goes to the GfxDevice — bits 0x20/0x40 of the same word
// pick its compute skinner (Internal-Skinning.compute, which fails here like
// every compute shader), and with those clear it asks for a pose buffer the
// GLES device cannot make ("Failed to create GPU skin pose buffer!"). Either
// way the mesh is never drawn: Iron Rebellion's enemy mechs, traced in the
// simulator with lldb. The GLES device sets the word from the version alone —
// `mov wN, #0xf1` / `str wN, [caps, #0x414]` right after the same ES-3.1-level
// test (`mov wM, #0x3838` mask) that sets the compute bit — and stores 0 below
// 3.1. Storing 0x90 drops bit 0 and the two variant bits and keeps the rest, so
// GPU skinning is off and Unity skins on the CPU, as it does on any device
// without it. 2019.4 has the same store at caps+0x408 (I Expect You To Die's
// libunity 0x3ed434, read by IsGPUSkinningEnabled at 0x50eb10), behind the
// level test written as `sub wT, wL, #0xb` / `cmp wT, #0x3`; both are taken.
// Without it, unity-no-compute alone leaves 2019.4 dispatching the skinning
// kernels that cannot compile, and its hands and lighter are never drawn.
static int cpuskin_find(kl_gp_at at, void *ctx, uint64_t *site, uint32_t *word,
                        const char **why) {
    unsigned n = 0;
    for (uint64_t va = 0; at(ctx, va); va += 4) {
        uint32_t w = *at(ctx, va);
        if ((w & 0xFFFFFFE0u) != 0x52801E20u) continue;              // mov wN, #0xf1
        unsigned rd = w & 31;
        int stored = 0;
        for (int s = 1; s <= 3 && !stored; s++) {
            uint32_t *ps = at(ctx, va + s * 4);
            stored = ps && (*ps & 0xFFC00000u) == 0xB9000000u && (*ps & 31) == rd;
        }
        int gated = 0;
        for (int b = 1; b <= 14 && !gated && va >= (uint64_t)b * 4; b++) {
            uint32_t *pb = at(ctx, va - b * 4);
            if (!pb) break;
            gated = (*pb & 0xFFFFFFE0u) == 0x52870700u;               // mov wM, #0x3838
            // 2019.4: `sub wT, wL, #0xb` / `cmp wT, #0x3` / `b.hs` — the same
            // ES-3.1-level test, written as a range check on the level enum.
            if (!gated && (*pb & 0xFFFFFC00u) == 0x51002C00u) {      // sub wT, wL, #0xb
                uint32_t *pc = at(ctx, va - b * 4 + 4);
                unsigned t = *pb & 31;
                gated = pc && *pc == (0x71000C1Fu | (t << 5));      // cmp wT, #0x3
            }
        }
        if (stored && gated) { if (!n) { *site = va; *word = w; } n++; }
    }
    if (n == 1) return 1;
    *why = n ? "the skinning-caps store appears more than once"
             : "no level-gated `mov wN, #0xf1` store";
    return 0;
}

static unsigned cpuskin_apply(const char *base, kl_gp_at at, void *ctx) {
    if (turned_off("unity-cpu-skinning")) {
        fprintf(stderr, "  [patch] unity-cpu-skinning: OFF (KL_GUEST_PATCH_OFF)\n");
        return 0;
    }
    uint64_t site = 0; uint32_t w = 0; const char *why = "?";
    if (!cpuskin_find(at, ctx, &site, &w, &why)) {
        fprintf(stderr, "  [patch] unity-cpu-skinning: SKIPPED — %s in %s; the image is "
                        "untouched\n", why, base);
        return 0;
    }
    uint32_t to = 0x52801200u | (w & 31);                             // mov wN, #0x90
    *at(ctx, site) = to;
    fprintf(stderr, "  [patch] unity-cpu-skinning: applied to %s — skinned meshes are "
                    "skinned on the CPU (0x%llx: 0x%08x -> 0x%08x)\n",
            base, (unsigned long long)site, w, to);
    return 1;
}

unsigned kl_guest_patch_apply(const char *path, kl_gp_at at, void *ctx) {
    if (!kl_env_on("KL_GUEST_PATCH", 1)) return 0;
    const char *base = basename_of(path);
    unsigned applied = 0;
    if (strcmp(base, "libunity.so") == 0) {
        applied += texunit_apply(base, at, ctx);
        applied += nocompute_apply(base, at, ctx);
        applied += cpuskin_apply(base, at, ctx);
    }

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
