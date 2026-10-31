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

// HITMAN 3 VR: Reloaded (libengine.so, Glacier, store target x8012387035448196,
// build 3641): the main loop caps itself at 36 frames a second.
//
// ZApplicationEngineQuest::MainLoop loads one float before its loop, 27.8 (ms),
// and after each frame, while GetVRRenderMode() is 1 and the OpenXR session
// runs, Sleep()s whatever is left of it. On the Quest that is the space-warp
// rate, 36 drawn for 72 shown. Here nobody fills in the frames between, and a
// 27.8 ms frame waits for the third 10 ms refresh: 33 fps at 100 Hz. Measured
// on the Mac (sample, 2026-10-20): the main thread spent 89% of its time in
// usleep under MainLoop, the render thread idled polling its command ring, the
// GPU took about 7 ms a frame.
//
// 14 ms instead: more than one refresh and less than two at both 100 and 120
// Hz, so the next xrWaitFrame takes the frame to the second refresh — 50 fps
// at 100 Hz, 60 at 120, the half rate the game was built for.
static const kl_gp_word k_hitman3_frame_cap[] = {
    { 0x31c0350, 0xbd41b108, 0x1e259008, "ldr s8, [x8, #0x1b0] (27.8) -> fmov s8, #14.0" },
};

// The Walking Dead: Saints & Sinners - Chapter 2: Retribution (libUE4.so, UE
// 4.27.2, version code 32342700, store target x5190288877703616): Niagara is
// told there is no compute.
//
// The level after the tutorial has a Niagara emitter that simulates on the
// GPU. Its first dispatch asks for a compute shader (a 64-wide kernel with the
// GroupSharedIndex compaction of Niagara's emitter shader), a stage the ES 3.0
// context here does not have: glCreateShader(GL_COMPUTE_SHADER) answers 0, the
// program links with "No compiled shaders.", FOpenGLDynamicRHI::
// GetLinkedComputeProgram logs its fatal (ForceQuit, ignored) and the render
// thread faults in BindPendingComputeShaderState — the loading screen stays up
// for good (on device, twice, 2026-10-17). Unreal never asks whether compute
// exists: the ES 3.1 shader platform has it by definition. The cvars that would
// switch it off (fx.NiagaraAllowGPUParticles, fx.NiagaraAllowComputeShaders)
// cannot be reached in this Shipping build — it has no -ini: or -dpcvars
// command-line override, and a command line carrying them changed nothing.
//
// So the two questions Niagara does ask are answered "no":
// FNiagaraUtilities::AllowGPUParticles and ::AllowComputeShaders return false.
// Their callers (FNiagaraEmitterInstance::IsAllowedToExecute, the batcher's
// PreInitViews/PostInitViews/PreRender/PostRenderOpaque,
// UNiagaraScript::DidScriptCompilationSucceed, InitEmitters, ...) then leave
// GPU-simulated emitters out, as they would on a platform without compute.
// CPU-simulated effects are untouched. The last words of each group are
// fingerprints only (the reads of the two cvars), which pin it to this build.
//   0x8347080  adrp x9, ...              -> mov w0, wzr   (AllowGPUParticles)
//   0x8347084  ldr  x9, [x9, #1136]      -> ret
//   0x8347184  adrp x9, ...              -> mov w0, wzr   (AllowComputeShaders)
//   0x8347188  ldr  x9, [x9, #1136]      -> ret
static const kl_gp_word k_twd2_niagara_no_compute[] = {
    { 0x8347080, 0xd0031729, 0x2a1f03e0, "AllowGPUParticles: mov w0, wzr" },
    { 0x8347084, 0xf9423929, 0xd65f03c0, "AllowGPUParticles: ret" },
    { 0x8347184, 0xd0031729, 0x2a1f03e0, "AllowComputeShaders: mov w0, wzr" },
    { 0x8347188, 0xf9423929, 0xd65f03c0, "AllowComputeShaders: ret" },
    { 0x8347130, 0xf9423d08, 0xf9423d08, "fingerprint only (fx.NiagaraAllowGPUParticles read)" },
    { 0x8347140, 0xf9424108, 0xf9424108, "fingerprint only (fx.NiagaraAllowComputeShaders read)" },
    { 0x8347234, 0xf9424108, 0xf9424108, "fingerprint only (fx.NiagaraAllowComputeShaders read)" },
};

// Wrath2 (libUE4.so) — the "COMPILING SHADERS" title segment is left to the game.
//
// AAW_CompileShadersTitleScreenSegment gates on
// FShaderPipelineCache::NumPrecompilesRemaining() reaching 0. A patch here
// ("wrath2-pso-done": 0xcded830 -> mov w0, #0; ret) used to answer 0 for it,
// from the time the precompile stalled at ~529 PSOs and the segment never
// ended. The precompile drains now, a pipeline per guest frame, and all the
// patch still did was end the game's own screen after ten seconds: the eye
// picture stayed black for the minutes the rest of the pipelines took, with
// nothing saying why. Without it (headset, 2026-10-13) the screen stood for
// 3 min 50 s while the count went from 3,600 to 8,980, and the game went on to
// its calibration by itself. Should the precompile stall again, the segment
// stays up and the log's "pipeline warmup" count stops climbing.

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

// ---- Half-Life: Alyx (Linux ARM64, depot 546564 build of 2026-09) -----------
//
// Source 2 hands LuaJIT its memory from below 2 GB. vscript's mmap override
// (lua_VALVE_OVERRIDE_MMAP) asks tier0's Plat_VirtualAlloc for a region with an
// upper address limit of 0x80000000; Plat_VirtualAlloc probes hint after hint in
// 64 KB steps, unmapping whatever the kernel returns above the limit, and
// vscript then checks the address once more itself. On Linux that finds a hole.
// On Darwin arm64 there is none to find: everything below 4 GB is __PAGEZERO,
// on a Mac and on a headset alike. So the probe fails after tens of thousands
// of mmap/munmap pairs and the engine dies with "Lua unable to create VM".
//
// The limit is policy, not need. This LuaJIT is a GC64 build — lua_gettop reads
// base and top at 0x20/0x28, the 64-bit GCRef layout — so it is correct for any
// address below 2^47, which is every address a Darwin process has. The two
// comparisons are therefore removed: Plat_VirtualAlloc takes the first mapping
// it is given, and vscript does not second-guess it.
//
// Taking the first mapping also ends the start-up storm: tier0's own static
// initialiser makes the same request for a 2 GB reserve, and used to burn
// ~390,000 syscalls discovering there was no room. Its caller still declines the
// high address, as it did before, and carries on without the reserve.
static const kl_gp_word k_alyx_valloc_limit[] = {
    { 0x70ef0, 0x54000309, 0x14000018, "b.ls -> b: an address above the caller's limit is accepted" },
};
static const kl_gp_word k_alyx_lua_limit[] = {
    { 0x4dfd8, 0x540005a8, 0xd503201f, "b.hi -> nop: Lua's arena may sit above 2 GB" },
};

// Half-Life: Alyx, libclient.so — UTIL_PlayerByUserId. It walks the players and
// returns the one whose engine user id equals the id an event carries, or NULL;
// and the handler for `player_teleport_finish` (libclient.so+0xffabd8) reads a
// byte off the result without looking. Here the two ids do not agree — the
// event's "userid" matches no player — so the FIRST teleport of a session ended
// it with a fault at address 0x3bd (measured on a Vision Pro, 2026-10-06).
//
// Why they disagree is not established; the suspect is the offline Steam stand-in
// (kl_lx_steam.c), which is what the engine fills its player info from. Until
// that is, the comparison is skipped: the function returns the first player
// there is, which in a single-player game is the only one and the one meant.
static const kl_gp_word k_alyx_player_by_userid[] = {
    { 0x1460bc8, 0x540000e0, 0x14000007, "b.eq -> b: the first player is the player" },
};

// Counter-Strike VR (Team Beef's CSVR 1.0.5, libxash.so) — where the flat HUD
// sits inside the eye image.
//
// In VR game mode the engine shrinks every 2D element that is not full-screen
// to a quarter and centres it (engine/client/cl_game.c, SPR_AdjustSize):
//
//     scale = 0.25;  offset = (1 - scale) / 2;        // 0.375
//     x = x * scale + iWidth  * offset;   w *= scale;
//     y = y * scale + iHeight * offset;   h *= scale;
//
// There is no cvar for either number and `hud_scale` is forced every frame, so
// there is nothing to answer from outside. On a Vision Pro the result is a HUD
// that fills the middle of the view; reported as too large and too high.
//
// The function exists once and is inlined at three more places (SPR_DrawGeneric,
// CL_FillRGBA, CL_FillRGBABlend). Each copy loads the offset ONCE and uses it
// for both axes, so lowering the HUD needs a third constant, i.e. one more
// instruction than the copy has. The slot comes from the end of the block: it
// closes with a `b` over the `scvtf` that the other branch uses to convert the
// screen size, and converting the same integers a second time changes nothing —
// so the body moves down one word, the `b` goes, and control falls through.
//
// New values: scale 0.1875 (three quarters of the old size), x offset 0.40625
// (still centred), y offset 0.46875 (centred would be 0.40625; this is 1/16 of
// the image lower). All three are exact fmov immediates.
//
// Untouched on purpose: the copy in CL_DrawLoadingOrPaused (0x115300), which
// ends in a real backward branch and has no slot — the loading/pause plaque
// stays centred at the old size — and the crosshair, which the client draws
// through TriAPI at the projected aim point and never passes through here.
static const kl_gp_word k_csvr_hud_place[] = {
    // SPR_AdjustSize (fonts, VGUI)
    { 0x1139d4, 0x1e2b1002, 0x1e2b5002, "fmov s2, #0.375 -> #0.40625 (x offset)" },
    { 0x1139dc, 0x1e2a1004, 0x1e291004, "fmov s4, #0.25 -> #0.1875 (scale)" },
    { 0x1139e8, 0x1e220822, 0x1e2bd002, "+ fmov s2, #0.46875 (y offset)" },
    { 0x1139ec, 0x1f040ca3, 0x1e220822, "fmul s2, s1, s2 (moved down one)" },
    { 0x1139f0, 0xbd0002c3, 0x1f040ca3, "fmadd s3, s5, s4, s3" },
    { 0x1139f4, 0xbd4002a3, 0xbd0002c3, "str s3, [x22]" },
    { 0x1139f8, 0x1f040862, 0xbd4002a3, "ldr s3, [x21]" },
    { 0x1139fc, 0xbd0002a2, 0x1f040862, "fmadd s2, s3, s4, s2" },
    { 0x113a00, 0xbd400282, 0xbd0002a2, "str s2, [x21]" },
    { 0x113a04, 0x1e240842, 0xbd400282, "ldr s2, [x20]" },
    { 0x113a08, 0xbd000282, 0x1e240842, "fmul s2, s2, s4" },
    { 0x113a0c, 0xbd400262, 0xbd000282, "str s2, [x20]" },
    { 0x113a10, 0x1e240842, 0xbd400262, "ldr s2, [x19]" },
    { 0x113a14, 0xbd000262, 0x1e240842, "fmul s2, s2, s4" },
    { 0x113a18, 0x14000003, 0xbd000262, "str s2, [x19]; the b is gone, falls into scvtf s0/s1" },
    // SPR_DrawGeneric (HUD sprites)
    { 0x115618, 0x1e2b1002, 0x1e2b5002, "x offset" },
    { 0x11561c, 0x1e2a1004, 0x1e291004, "scale" },
    { 0x115628, 0x1e220802, 0x1e2bd002, "+ fmov s2, #0.46875 (y offset)" },
    { 0x11562c, 0x1e24094a, 0x1e220802, "fmul s2, s0, s2" },
    { 0x115630, 0x1f040def, 0x1e24094a, "fmul s10, s10, s4" },
    { 0x115634, 0x1f040908, 0x1f040def, "fmadd s15, s15, s4, s3" },
    { 0x115638, 0x14000002, 0x1f040908, "fmadd s8, s8, s4, s2; falls into scvtf s0" },
    // CL_FillRGBA
    { 0x117b74, 0x1e2b1002, 0x1e2b5002, "x offset" },
    { 0x117b78, 0x1e2a1004, 0x1e291004, "scale" },
    { 0x117b84, 0x1e220802, 0x1e2bd002, "+ fmov s2, #0.46875 (y offset)" },
    { 0x117b88, 0x1e24094a, 0x1e220802, "fmul s2, s0, s2" },
    { 0x117b8c, 0x1f040d29, 0x1e24094a, "fmul s10, s10, s4" },
    { 0x117b90, 0x1f04096b, 0x1f040d29, "fmadd s9, s9, s4, s3" },
    { 0x117b94, 0x14000002, 0x1f04096b, "fmadd s11, s11, s4, s2; falls into scvtf s0" },
    // CL_FillRGBABlend
    { 0x119cf4, 0x1e2b1002, 0x1e2b5002, "x offset" },
    { 0x119cf8, 0x1e2a1004, 0x1e291004, "scale" },
    { 0x119d04, 0x1e220802, 0x1e2bd002, "+ fmov s2, #0.46875 (y offset)" },
    { 0x119d08, 0x1e24094a, 0x1e220802, "fmul s2, s0, s2" },
    { 0x119d0c, 0x1f040d29, 0x1e24094a, "fmul s10, s10, s4" },
    { 0x119d10, 0x1f04096b, 0x1f040d29, "fmadd s9, s9, s4, s3" },
    { 0x119d14, 0x14000002, 0x1f04096b, "fmadd s11, s11, s4, s2; falls into scvtf s0" },
};

// Pistol Whip 1.5.73.2104 (x2104963472963790) — tolerate unavailable end-of-run
// score state. Sent in by a user (fix 27); measured by them, not by the project.
//
// Returning from gameplay invokes ScoreManager.OnNewGameScore (metadata method
// RID 9388, libil2cpp.so 0x11e5ad0). Here one of the optional score/platform
// objects is absent. Every generated null guard in this method converges at
// 0x11e6080, which raises NullReferenceException; the exception escapes into
// the scene-transition coroutine before FadeFromColor and the loading-screen
// cleanup can run, and the fade stays black. The native exception-boundary
// trace measured LR=libil2cpp.so+0x11e6084, so this sink is the one reached.
//
// The missing state is treated as "score finalization unavailable": the sink
// branches back to the method's own epilogue (0x11e6064, seven instructions
// up) instead of calling the raise helper. What ran before the guard stays
// done and the enclosing transition finishes. No score is made up or sent.
static const kl_gp_word k_pistol_whip_score_null_exit[] = {
    { 0x11e6080, 0x97f6c9f4, 0x17fffff9,
      "bl <raise NullReferenceException> -> b 0x11e6064: OnNewGameScore's normal return" },
};

// Shave & Stuff 1.12.3.2 (x7532973430077887) — permit the selected base
// scenario to start when the optional remote configuration service is absent.
//
// The title reaches its character/salon selector and accepts controller input,
// but GameManager.CanStartGame remains false after Firebase/Gamestoty remote
// configuration fails on this host.  The property checks three pieces of
// already-created local state and is used only as the readiness gate before
// starting the scenario currently selected by the game.  Returning true here
// does not select a scenario, manufacture an entitlement, or touch IAP state;
// locked salons and DLC continue to be decided by their own data.
//
// The two prologue words alone are a weak fingerprint — `str x20, [sp, #-0x20]!;
// stp x19, x30, [sp, #0x10]` opens a great many functions, and every guest with
// a libil2cpp.so is checked against this row. So the group also carries
// CharacterSelectionPanel's eight words below as unchanged rows (expect == to):
// it applies only where both sites match, which pins it to this build. The rows
// read the ORIGINAL words, so this group sits ahead of the one that rewrites them.
static const kl_gp_word k_shave_stuff_can_start_offline[] = {
    { 0x268af68, 0xf81e0ff4, 0x52800020,
      "GameManager.CanStartGame: mov w0, #1 when remote config is unavailable" },
    { 0x268af6c, 0xa9017bf3, 0xd65f03c0,
      "GameManager.CanStartGame: return selected local scenario readiness" },
    { 0x2660d0c, 0xf9402a74, 0xf9402a74, "fingerprint only (CharacterSelectionPanel)" },
    { 0x2660d10, 0xaa1303e0, 0xaa1303e0, "fingerprint only" },
    { 0x2660d14, 0xaa1f03e1, 0xaa1f03e1, "fingerprint only" },
    { 0x2660d18, 0x94295c3e, 0x94295c3e, "fingerprint only" },
    { 0x2660d1c, 0xb4000560, 0xb4000560, "fingerprint only" },
    { 0x2660d20, 0xaa1f03e1, 0xaa1f03e1, "fingerprint only" },
    { 0x2660d24, 0x94297ecf, 0x94297ecf, "fingerprint only" },
    { 0x2660d28, 0xb0010773, 0xb0010773, "fingerprint only" },
};

// The unlocked Barbershop card does reach CharacterSelectionPanel.OnPointerClick
// and CreativeModeManager.PickCharacter: the selection and its progress state
// are already committed at 0x2660d08.  Immediately afterwards the panel tries
// to instantiate its optional selectionFxPrefab.  This serialized reference is
// null in the Quest asset set as loaded on this host, so every generated null
// guard converges on 0x2660dc8 and raises NullReferenceException.  The exception
// escapes the pointer callback and leaves the selector's busy ring on screen.
//
// Return normally immediately after PickCharacter.  This drops only that
// cosmetic selection effect; it does not touch ChapterDreads (the sole value
// of IapPurchaseItem), chapter progression, saved unlocks, or purchase state.
// The eight-word epilogue is copied from this method's own normal ABI restore.
static const kl_gp_word k_shave_stuff_missing_selection_fx[] = {
    { 0x2660d0c, 0xf9402a74, 0xa9457bf3, "restore x19/x30 after PickCharacter" },
    { 0x2660d10, 0xaa1303e0, 0xa94453f5, "restore x21/x20 after PickCharacter" },
    { 0x2660d14, 0xaa1f03e1, 0xf9401ff6, "restore x22 after PickCharacter" },
    { 0x2660d18, 0x94295c3e, 0x6d42a3e9, "restore d9/d8 after PickCharacter" },
    { 0x2660d1c, 0xb4000560, 0x6d41abeb, "restore d11/d10 after PickCharacter" },
    { 0x2660d20, 0xaa1f03e1, 0x6d40b3ed, "restore d13/d12 after PickCharacter" },
    { 0x2660d24, 0x94297ecf, 0xfc4607ee, "restore d14 and stack after PickCharacter" },
    { 0x2660d28, 0xb0010773, 0xd65f03c0, "return without optional selection effect" },
};

// Medal of Honor: Above and Beyond (libUE4.so, UE 4.25, store target
// x3647259232032222): the game's own Quit tears the RHI down twice.
//
// AndroidMain ends with FEngineLoop::Exit() and then RequestExit(true), i.e.
// ForceQuit. Inside Exit, FShaderCodeLibrary::Shutdown() frees the shader code
// archive, and RHIExit() -> FVulkanDynamicRHI::Shutdown() ->
// FRHIResource::FlushPendingDeletes() then runs ~FShaderCodeArchive on it a
// second time: FMallocBinned2::Free reads the pool header of a page that has
// already gone back to the system, and the game thread faults (his headset,
// 2026-10-18 02:56, a second after "Quit"; symbolised from the build's own
// exports). On a Quest that fault lands in a process that is about to end and
// nobody sees it. Here it is a crash in front of the player.
//
// The RHI is the last thing Exit tears down before the movie player and the
// task graph, and the process ends right after; skip only that call. Settings
// and saves are written by the game as they change (SaveConfig flushes), not
// here. The neighbouring calls are fingerprints, which pins it to this build.
//   0x685236c  bl RHIExit  -> nop
static const kl_gp_word k_moh_exit_no_rhi_teardown[] = {
    { 0x685236c, 0x94754624, 0xd503201f, "FEngineLoop::Exit: skip RHIExit (nop)" },
    { 0x6852368, 0x9450db4f, 0x9450db4f, "fingerprint only (bl FGraphEvent::Recycle)" },
    { 0x6852370, 0x94beaafe, 0x94beaafe, "fingerprint only (bl DestroyMoviePlayer)" },
    { 0x684f3b0, 0x94000ad8, 0x94000ad8, "fingerprint only (AndroidMain: bl FEngineLoop::Exit)" },
    { 0x684f3b8, 0x9450a993, 0x9450a993, "fingerprint only (AndroidMain: bl RequestExit)" },
};

// Fluid Browser + Video Player (com.Fluid.Fluid 0.44.6.1, version code 845,
// store target x6946182742093178; Unity 6000.3, metadata v39): the startup
// sequence's "Keyboard Warmup" step never completes, so the app stays on its
// logo with the progress bar full (2026-10-21).
//
// StartupLoadSequence.KeyboardWarmupStep waits for its SurfaceAttached
// property, which is
//
//     get => <forced> || (keyboard != null && keyboard.WarmupSurfaceAttached);
//
// and FluidVirtualKeyboard.WarmupSurfaceAttached is set only when the Java
// keyboard (so/fluid/kb/FluidKeyboard) attaches its Android surface. There is
// no Java here, so it never is. The route: metadata v39 names the methods
// (KeyboardWarmupStep::get_SurfaceAttached is token 0x06001b2d), and
// Assembly-CSharp.dll's Il2CppCodeGenModule methodPointers[rid - 1] gives
// 0x443564c. Taking the <forced> edge makes the step complete; the keyboard
// itself stays without its surface, as it would anyway.
//   0x443567c  cbz w8, 0x4435688  -> nop     ; SurfaceAttached -> true
static const kl_gp_word k_fluid_keyboard_warmup[] = {
    { 0x443567c, 0x34000068, 0xd503201f, "KeyboardWarmupStep::get_SurfaceAttached -> true (nop)" },
    { 0x443564c, 0xf81e0ffe, 0xf81e0ffe, "fingerprint only (get_SurfaceAttached prologue)" },
    { 0x4435678, 0x39406268, 0x39406268, "fingerprint only (ldrb w8, [x19, #0x18])" },
    { 0x4435680, 0x52800020, 0x52800020, "fingerprint only (mov w0, #1)" },
    { 0x44883c0, 0x3942c800, 0x3942c800, "fingerprint only (FluidVirtualKeyboard::get_WarmupSurfaceAttached)" },
};

// Roboquest VR, Meta Store build (libUnreal.so, UE 5.6.1 in Meta's fork; store
// target x9269393463181825, version 1.0.4 / 101): FMallocBinned3's pools are
// 512 MB here instead of 1 GB, so its one reservation fits this address space.
//
// The engine's allocator is created by the first static initialiser that
// allocates, and FMallocBinned3's constructor reserves one block of address
// space for all of its pools at once: 76 pools (size classes up to 128 KB) of
// 1 GB each, 76 GB, mmap'd read-write. Android overcommits that. visionOS does
// not have the addresses: even with extended-virtual-addressing (the app has
// it) the ceiling is about 64 GB, the mmap fails, and Unreal stops with "Ran
// out of memory allocating 81604378624 bytes" — inside static init, before
// GError exists, so the report itself faults (write at 0x10, pc
// libUnreal.so+0x1f5b638; crash reports 2026-10-23 21:34 and 21:43, on start).
//
// The pool size is a compile-time constant, so it is in the code in a few
// spellings; all of them are here. Every reader of the allocator's base
// (the static at 0xa3ad960, read directly at 4 sites and through its GOT slot
// 0xa207b00 at 5 — a scan of the whole .text, 2026-10-23) is in this one
// region, and these are the pool-size uses around them:
//   * pool index from an offset: lsr/ubfx #30 -> #29
//   * pool base from an index:   lsl #30 -> #29; the pool-start mask
//     0x3fffffffc0000000 -> 0x3fffffffe0000000
//   * "is this an OS allocation": (offset >> 32) against 0x12/0x13, i.e.
//     offset >= 76 GB, becomes (offset >> 31) against the same numbers, i.e.
//     offset >= 38 GB = 76 pools of 512 MB
//   * blocks per pool, which sizes each pool's bitmaps: 1 GB / block -> 512 MB
//   * the reservation: 76 GB -> 40 GB (38 GB rounded up to what one movz
//     can say; the last 2 GB is never handed out and never looked up), and the
//     extent the constructor registers with it.
// A size class now holds up to 512 MB; the whole game is told it has 6 GB
// (KL_MEM_TOTAL_MB), and no one size class of an allocator spread over 76 comes
// near that. The pool count, the size classes and everything else stay as they
// were.
static const kl_gp_word k_roboquest_meta_binned3[] = {
    { 0x1e97e30, 0x92627d6b, 0x9263816b, "and x11, x11, #0x3fffffffc0000000 -> #0x3fffffffe0000000 (start of the pool a block is in)" },
    { 0x1e98200, 0xd3627edb, 0xd3637edb, "ubfiz x27, x22, #30, #32 -> #29 (pool index -> pool base)" },
    { 0x1e9862c, 0xd2c00260, 0xd2c00140, "mov x0, #0x1300000000 -> #0xa00000000 (the reservation: 76 GB -> 40 GB)" },
    { 0x1e9865c, 0x92dffda9, 0x92dffec9, "mov x9, #-0xffed00000001 -> #-0xfff600000001 (its extent: 76 GB - 1 -> 40 GB - 1)" },
    { 0x1e986c4, 0x52a8000d, 0x52a4000d, "mov w13, #0x40000000 -> #0x20000000 (blocks per pool, for the bitmaps' size)" },
    { 0x1e9883c, 0x52a80009, 0x52a40009, "mov w9, #0x40000000 -> #0x20000000 (blocks per pool)" },
    { 0x1e99680, 0x8b147929, 0x8b147529, "add x9, x9, x20, lsl #30 -> lsl #29 (pool index -> pool base)" },
    { 0x1e99e48, 0xd35efd14, 0xd35dfd14, "lsr x20, x8, #30 -> #29 (pool index)" },
    { 0x1e99e50, 0xd360fd08, 0xd35ffd08, "lsr x8, x8, #32 -> #31 (OS allocation: offset >= 38 GB)" },
    { 0x1e9cb1c, 0xd360fea8, 0xd35ffea8, "lsr x8, x21, #32 -> #31 (OS allocation: offset >= 38 GB)" },
    { 0x1e9cb38, 0xd35efea8, 0xd35dfea8, "lsr x8, x21, #30 -> #29 (pool index)" },
    { 0x1e9cb98, 0xd35ef6a8, 0xd35df2a8, "ubfx x8, x21, #30, #32 -> #29, #32 (pool index)" },
    { 0x1e9cc50, 0xd35ef6a8, 0xd35df2a8, "ubfx x8, x21, #30, #32 -> #29, #32 (pool index)" },
    { 0x1e9cca0, 0xd35ef6a8, 0xd35df2a8, "ubfx x8, x21, #30, #32 -> #29, #32 (pool index)" },
    { 0x1e9cd34, 0xd360fd09, 0xd35ffd09, "lsr x9, x8, #32 -> #31 (OS allocation: offset >= 38 GB)" },
    { 0x1e9cd38, 0xd35efd02, 0xd35dfd02, "lsr x2, x8, #30 -> #29 (pool index)" },
    { 0x1e9cf14, 0xd360fd09, 0xd35ffd09, "lsr x9, x8, #32 -> #31 (OS allocation: offset >= 38 GB)" },
    { 0x1e9cf24, 0xd35efd08, 0xd35dfd08, "lsr x8, x8, #30 -> #29 (pool index)" },
    { 0x1e9d5d4, 0xd360fd09, 0xd35ffd09, "lsr x9, x8, #32 -> #31 (OS allocation: offset >= 38 GB)" },
    { 0x1e9d5e4, 0xd35efd08, 0xd35dfd08, "lsr x8, x8, #30 -> #29 (pool index)" },
    { 0x1e986e4, 0xf101313f, 0xf101313f, "fingerprint only (cmp x9, #0x4c: 76 pools)" },
    { 0x1e99e58, 0xf100491f, 0xf100491f, "fingerprint only (cmp x8, #0x12, kept: (offset >> 31) > 0x12 is offset >= 38 GB)" },
    { 0x1e9cb20, 0xfa531900, 0xfa531900, "fingerprint only (ccmp x8, #0x13, kept: (offset >> 31) < 0x13 is offset < 38 GB)" },
    { 0x1e9cd3c, 0xf100493f, 0xf100493f, "fingerprint only (cmp x9, #0x12, kept)" },
    { 0x1e9cf18, 0xf100493f, 0xf100493f, "fingerprint only (cmp x9, #0x12, kept)" },
    { 0x1e9d5d8, 0xf100493f, 0xf100493f, "fingerprint only (cmp x9, #0x12, kept)" },
};

// Roboquest VR, Meta Store build (libUnreal.so, UE 5.6.1 in Meta's fork; store
// target x9269393463181825, version 1.0.4 / 101): FPakPlatformFile::OpenMappedEx
// gives the engine's own answer for a platform without memory-mapped IO. The
// same patch as the earlier Roboquest build's (x3151760, 1.0.3: the pak layer
// of that build, measured 2026-10-22), re-measured for this one.
//
// The game's one pak is inside its patch OBB and reaches the engine through the
// read-through (kl_obbmap). Those fds cannot be memory-mapped — the pak starts
// at an arbitrary offset in the archive — so klb_mmap refuses them with ENODEV.
// UE 5.6's async loader maps bulk data through FPakPlatformFile::OpenMappedEx,
// the open itself succeeds (it is only open + fstat underneath), and the pak's
// MapRegion uses the region FAndroidMappedFileHandle::MapRegion hands back
// without checking it for null: a read at 0x8 on the FAsyncLoading thread a
// second after the engine came up (crash report 2026-10-23 22:50:42,
// libUnreal.so 0x32e16dc: ldp x10, x8, [x21, #0x8] on the null region).
//
// `mmio.enable` is the engine's switch for exactly this case, and OpenMappedEx
// reads it first — but this Shipping build has no way to set it from outside.
// So the branch on it is taken always: every pak map request answers "Memory
// mapped IO is not enabled on this platform", the caller already handles that
// error, and the bulk data is read the ordinary way through the read-through.
// The registration row pins the variable the branch tests to the cvar's name.
//   0x32c7254  cbz w9, 0x32c7338  -> b 0x32c7338
static const kl_gp_word k_roboquest_meta_pak_no_mmio[] = {
    { 0x32c7254, 0x34000729, 0x14000039, "FPakPlatformFile::OpenMappedEx: cbz mmio.enable -> b (not enabled)" },
    { 0x32c724c, 0xb0038549, 0xb0038549, "fingerprint only (adrp of mmio.enable)" },
    { 0x32c7250, 0xb942bd29, 0xb942bd29, "fingerprint only (ldr w9, mmio.enable)" },
    { 0x32c7338, 0x90fe7a61, 0x90fe7a61, "fingerprint only (adrp of \"Memory mapped IO is not enabled\")" },
    { 0x32f167c, 0x910af042, 0x910af042, "fingerprint only (mmio.enable's registration: the variable it binds)" },
};

static const kl_gp_patch k_patches[] = {
    { "roboquest-meta-pak-no-mmio", "libUnreal.so",
      "pak bulk data is read, not memory-mapped (mmio.enable's own path): the read-through cannot map it",
      k_roboquest_meta_pak_no_mmio, sizeof k_roboquest_meta_pak_no_mmio / sizeof *k_roboquest_meta_pak_no_mmio },
    { "roboquest-meta-binned3-512m", "libUnreal.so",
      "the allocator's pools are 512 MB, so its start-up reservation is 40 GB, not 76",
      k_roboquest_meta_binned3, sizeof k_roboquest_meta_binned3 / sizeof *k_roboquest_meta_binned3 },
    { "fluid-keyboard-warmup", "libil2cpp.so",
      "the startup keyboard warmup completes without the Java keyboard's surface",
      k_fluid_keyboard_warmup,
      sizeof k_fluid_keyboard_warmup / sizeof *k_fluid_keyboard_warmup },
    { "moh-exit-no-rhi-teardown", "libUE4.so",
      "Quit does not tear the Vulkan RHI down a second time (shader archive double free)",
      k_moh_exit_no_rhi_teardown,
      sizeof k_moh_exit_no_rhi_teardown / sizeof *k_moh_exit_no_rhi_teardown },
    // Order matters: can-start-offline fingerprints the words selection-fx rewrites.
    { "shave-stuff-can-start-offline", "libil2cpp.so",
      "the already-selected base salon can start without optional remote config",
      k_shave_stuff_can_start_offline,
      sizeof k_shave_stuff_can_start_offline / sizeof *k_shave_stuff_can_start_offline },
    { "shave-stuff-missing-selection-fx", "libil2cpp.so",
      "a missing cosmetic selection prefab does not abort the selected base salon",
      k_shave_stuff_missing_selection_fx,
      sizeof k_shave_stuff_missing_selection_fx / sizeof *k_shave_stuff_missing_selection_fx },
    { "pistol-whip-score-null-exit", "libil2cpp.so",
      "missing optional end-of-run score state does not abort the scene transition",
      k_pistol_whip_score_null_exit,
      sizeof k_pistol_whip_score_null_exit / sizeof *k_pistol_whip_score_null_exit },
    { "csvr-hud-smaller-lower", "libxash.so",
      "the VR HUD is drawn at 3/4 of its size and 1/16 of the image lower",
      k_csvr_hud_place, sizeof k_csvr_hud_place / sizeof *k_csvr_hud_place },
    { "alyx-player-by-userid", "libclient.so",
      "an event's userid resolves to the single player even when the ids disagree",
      k_alyx_player_by_userid, sizeof k_alyx_player_by_userid / sizeof *k_alyx_player_by_userid },
    { "alyx-virtualalloc-no-address-limit", "libtier0.so",
      "Plat_VirtualAlloc does not insist on memory below 2 GB, which Darwin cannot give",
      k_alyx_valloc_limit, sizeof k_alyx_valloc_limit / sizeof *k_alyx_valloc_limit },
    { "alyx-lua-arena-anywhere", "libvscript.so",
      "the Lua VM's arena is accepted wherever it is mapped (GC64 LuaJIT)",
      k_alyx_lua_limit, sizeof k_alyx_lua_limit / sizeof *k_alyx_lua_limit },
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

    { "twd2-niagara-no-compute", "libUE4.so",
      "Niagara's AllowGPUParticles/AllowComputeShaders answer false: no compute stage here",
      k_twd2_niagara_no_compute,
      sizeof k_twd2_niagara_no_compute / sizeof *k_twd2_niagara_no_compute },
    { "wrath2-force-vulkan-available", "libUE4.so",
      "FAndroidMisc::IsVulkanAvailable() always returns true (the RHI gate)",
      k_wrath2_forcevk, sizeof k_wrath2_forcevk / sizeof *k_wrath2_forcevk },

    { "hitman3-frame-cap", "libengine.so",
      "the main loop's own frame cap is 14 ms instead of 27.8 (half the refresh, not 36 fps)",
      k_hitman3_frame_cap, sizeof k_hitman3_frame_cap / sizeof *k_hitman3_frame_cap },

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
// unity-texunit-mask turns it off; on a headset that has to be in klepton-ld's
// environment at the build, because its __TEXT is signed and written once.

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

unsigned kl_guest_patch_apply(const char *path, kl_gp_at at, void *ctx) {
    if (!kl_env_on("KL_GUEST_PATCH", 1)) return 0;
    const char *base = basename_of(path);
    unsigned applied = 0;
    if (strcmp(base, "libunity.so") == 0) applied += texunit_apply(base, at, ctx);

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
