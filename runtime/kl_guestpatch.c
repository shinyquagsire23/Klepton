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
// NOT YET WIRED FOR DEVICE: a klepton-ld dylib is signed, so its text cannot be
// written at run time — the same table has to be applied OFFLINE, by
// tools/klepton_ld.c, which is why the entry point takes a resolver instead of
// a base pointer. Until that is done a device run of a patched target behaves
// like `KL_GUEST_PATCH=0`, which is a difference between the host and the
// headset and is called out here rather than discovered there.
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
// to the method. See notes/VRCHAT.md, Session 8.

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

// (3) The stereo rendering mode, in libUnityOpenXR.so rather than in the
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

static const kl_gp_patch k_patches[] = {
    { "vrchat-multipass", "libUnityOpenXR.so",
      "the stereo rendering mode is MultiPass, because there is no multiview",
      k_vrc_multipass, sizeof k_vrc_multipass / sizeof *k_vrc_multipass },
    { "vrchat-under-construction", "libil2cpp.so",
      "the VRCFlowManager wall coroutine completes instead of hanging",
      k_vrc_wall, sizeof k_vrc_wall / sizeof *k_vrc_wall },
    { "vrchat-min-client-version", "libil2cpp.so",
      "the client build number always compares new enough",
      k_vrc_minver, sizeof k_vrc_minver / sizeof *k_vrc_minver },
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
