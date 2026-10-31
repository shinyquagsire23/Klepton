// Measured RUNTIME pokes into a live guest object — the half of kl_guestpatch.c
// that cannot be done offline.
//
// kl_guestpatch.c rewrites INSTRUCTIONS, which is a decision already taken
// inside the binary, and it can be applied either at load or by klepton-ld. The
// thing here is different in kind: it is a store into a FIELD of a live engine
// object that does not exist until the guest has built it, so there is no image
// to edit and no offline moment to do it in — it has to happen at run time,
// after the graphics device is up.
//
// It lived in mains/m_boot.c until 2026-08-14, which meant the visionOS app
// never did it at all: on a headset the cap stayed at Unity's own 32, the binds
// past it were refused, and the samplers read stale unit-0 textures — i.e.
// every glyph rendered as a solid block, on device only, with the host clean.
// That is exactly the shape a driver-local workaround has, so it is here now
// and BOTH drivers call it.
#include "kl_guestpoke.h"
#include "klepton.h"
#include "kl_guestpatch.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

// ---------------------------------------------------------------------------
// Guest-shape probes.
//
// Anything derived by reverse engineering ONE libunity build has to say which
// build it was derived from, or it silently becomes a wild pointer against the
// next one. Beat Saber 1.6.0 (Unity 2018.4.4f1) is 15.7 MB where the 2019.4
// build is 17 MB, so every measured offset in this file moved -- the
// texture-unit cap poke below read a garbage pointer and took the whole
// lifecycle down with SIGSEGV in OUR code, which reads like a shim bug rather
// than like a constant that expired.

// Matches 20NN.N[N].N[N]<a|b|f|p>N... at b+i, and returns the length of the
// match, or 0. `want_rev` additionally requires the '_<revision>' suffix that
// makes a stamp the build's own rather than a version merely mentioned.
// Unity 6 counts differently — "6000.3.22f1_1c726e1fb402" — and is matched as
// well; read as "20NN" only, such a build reported the decoy "2018.3.0a1".
static size_t unity_version_at(const char *b, size_t n, size_t i, int want_rev) {
    if (i + 10 >= n) return 0;
    if ((b[i] != '2' && b[i] != '6') || b[i + 1] != '0') return 0;
    size_t j = i + 2;
    if (!isdigit((unsigned char)b[j]) || !isdigit((unsigned char)b[j + 1])) return 0;
    j += 2;
    if (j >= n || b[j] != '.') return 0;
    j++;
    size_t d0 = j; while (j < n && isdigit((unsigned char)b[j])) j++;
    if (j == d0 || j >= n || b[j] != '.') return 0;
    j++;
    size_t d1 = j; while (j < n && isdigit((unsigned char)b[j])) j++;
    if (j == d1 || j >= n) return 0;
    if (b[j] != 'f' && b[j] != 'p' && b[j] != 'a' && b[j] != 'b') return 0;
    j++;
    size_t d2 = j; while (j < n && isdigit((unsigned char)b[j])) j++;
    if (j == d2 || j >= n) return 0;
    size_t vlen = j - i;                       // the version, without any suffix
    // A FORK of the engine stamps a flavour between the version and the
    // revision: VRChat's is "2022.3.22f2-DWR_0c82e3992b68". Without this the
    // stamp fails to parse, the bare-string fallback takes over, and it finds
    // the same decoy "2018.3.0a1" the comment below describes -- so a build
    // whose version IS in the row table reports as an unlisted one and is
    // silently left alone. The flavour is kept in the returned string rather
    // than skipped, because it is part of the build's identity: two 2022.3
    // builds have different offsets, and the row table is keyed on this.
    if (b[j] == '-') {
        size_t f0 = j++;
        while (j < n && (isalnum((unsigned char)b[j]) || b[j] == '-' || b[j] == '.')) j++;
        if (j == f0 + 1) return 0;             // a bare '-' is not a flavour
        vlen = j - i;
    }
    if (!want_rev) return b[j] == '\0' ? vlen : 0;
    // The build stamp: '_' then hex revision digits, then the NUL.
    if (b[j] != '_') return 0;
    j++;
    size_t r0 = j;
    while (j < n && isxdigit((unsigned char)b[j])) j++;
    if (j - r0 < 8 || j >= n || b[j] != '\0') return 0;
    return vlen;
}

// Unity stamps its own version into libunity as a plain NUL-terminated string.
// libunity is stripped of everything useful -- 292 dynamic symbols and not one
// GfxDevice among them -- so scanning for the stamp is the only version signal
// available without a symbol table. One linear pass over the mapped image, once,
// at pump start.
//
// **A bare version string is not necessarily this build's version.** Beat Saber
// 1.40's libunity is Unity 2022.3.33f1 and also contains the literal
// "2018.3.0a1" at a LOWER address, so taking the first match reported the guest
// as an eight-year-old alpha. That is a wrong answer that looks like a right one,
// and everything keyed on the version inherits it.
//
// So the canonical BUILD STAMP is preferred: Unity writes the version with its
// source revision attached ("2022.3.33f1_b2c853adf198"), and a version that
// carries a revision is one this binary was built from rather than one it merely
// mentions. The bare-string scan stays as the fallback, so a guest whose libunity
// has no stamped revision behaves exactly as it did before.
// Does the LOADED libunity carry unity-texunit-mask (kl_guestpatch.c)? Asked
// of the image as it is mapped, not of a flag: on the headset the patch was
// written offline by klepton-ld and nothing at run time remembers that, and on
// the host the loader may have been told KL_GUEST_PATCH=0. kl_glfb masks
// sampler units only when this says yes, because the two halves have to agree
// — a shader sampling unit 3 while Unity bound the texture to 35 is the same
// black surface the patch exists to fix.
struct tu_ctx { const uint8_t *base; size_t span; };
static uint32_t *tu_at(void *c_, uint64_t va) {
    struct tu_ctx *c = c_;
    if (va + 4 > c->span) return NULL;
    return (uint32_t *)(uintptr_t)(c->base + va);
}
int kl_texunit_mask_active(void) {
    static int v = -1;
    if (v >= 0) return v;
    kl_image *u = kl_find_image("libunity.so");
    if (!u) return 0;                              // asked too early: do not cache
    struct tu_ctx c = { (const uint8_t *)kl_base(u), kl_span(u) };
    v = kl_gp_texunit_state(tu_at, &c) == 2;
    fprintf(stderr, "  [poke] texture units: %s\n", v
            ? "libunity masks them & 31 at the bind; sampler uniforms are masked to match"
            : "libunity is not masked (unity-texunit-mask not applied); sampler units pass through");
    return v;
}

static const char *unity_version(void) {
    static char ver[32];
    static int done;
    if (done) return ver[0] ? ver : NULL;
    done = 1;
    kl_image *u = kl_find_image("libunity.so");
    if (!u) return NULL;
    const char *b = (const char *)kl_base(u);
    size_t n = kl_span(u);
    for (int want_rev = 1; want_rev >= 0; want_rev--) {
        for (size_t i = 0; i + 10 < n; i++) {
            size_t len = unity_version_at(b, n, i, want_rev);
            if (!len || len >= sizeof ver) continue;
            memcpy(ver, b + i, len);
            ver[len] = 0;
            return ver;
        }
    }
    return NULL;
}

// Is [p, p+len) actually mapped? mincore reports ENOMEM for a range that is
// not, which is the cheapest honest test that does not need Mach. Used to stop
// a stale measured offset from being dereferenced rather than to prove it is
// the RIGHT object -- that is what the version gate is for.
static int addr_mapped(const void *p, size_t len) {
    if (!p) return 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) return 0;
    uintptr_t a   = (uintptr_t)p & ~((uintptr_t)pg - 1);
    uintptr_t end = ((uintptr_t)p + len + (uintptr_t)pg - 1) & ~((uintptr_t)pg - 1);
    size_t pages  = (size_t)((end - a) / (uintptr_t)pg);
    char vec[16];
    if (pages == 0 || pages > sizeof vec) return 0;
    return mincore((void *)a, (size_t)(end - a), vec) == 0;
}

// libunity's texture-unit cap, direct from the horse's mouth: a singleton getter
// returns *(base + <singleton>), and GfxDeviceGLES's SetTexture rejects units
// >= *(singleton + <field>) with "OpenGL Error: Invalid texture unit!".
// Unity defaults it to 32 without ever querying GL, while its HLSLCC-baked
// sampler bindings reach unit 35 on the post passes -- so the reject preempted
// the binds and the samplers read stale unit-0 textures.
//
// **How to measure a new row**, because it is the same four commands every time
// and each one is checkable:
//   1. `strings -a -t x libunity.so | grep 'Invalid texture unit'`  -> the string
//   2. `tools/gxref.py libunity.so --to=<that offset>`              -> who builds it
//   3. disassemble backwards from there to the guard: `bl <getter> / ldr wN,
//      [x0, #<field>] / cmp / b.ls <the string's block>`            -> the FIELD
//   4. disassemble <getter>: `adrp x8, <page> / ldr x0, [x8, #<lo>]`-> the SINGLETON
// Both numbers are then vaddrs in that library and nothing else has to be taken
// on faith.
//
// BOTH offsets are per-Unity-build and meaningless against any other, so the
// version is a GATE and not a comment. A title whose version is not listed is
// left alone and told so by name: skipping costs at worst the stale-texture
// artifact this works around, while poking costs a 4-byte store through a
// garbage pointer, which is unrecoverable and lands nowhere near the cause.
//
// TODO: DELETE THIS. It is a store into a reverse-engineered field of a private
// engine object, and it cannot be made to generalise -- there is no symbol to
// find it by (libunity is stripped) and the offset moves with every Unity
// build, so the version table can only ever grow one painful measurement at a
// time. It exists to work around Unity capping texture units at 32 without
// asking GL, while its own baked sampler bindings reach unit 35. The real fixes
// are upstream of it and either one retires this outright:
//   - have the guest ask, and answer honestly: find why libunity never queries
//     GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS here and make the query happen, or
//   - handle it where the reject is observed: GfxDeviceGLES refuses the bind
//     and logs "Invalid texture unit!", which kl_glfb can see -- remapping the
//     out-of-range unit at bind time needs no offset into anything.
// Until then it is OFF for every guest but the ones it was measured on, which is
// the only honest state for a constant nobody can verify.
//
// One row per measured (Unity version prefix, singleton vaddr, cap field
// offset) and whether the raise is USED on that build. A row with `use` 0 keeps
// the measurement while declining to store through it.
//
// Beat Saber's two rows are measured and NOT used. Unity's own cap check is
// load-bearing as a guard: at 32 the engine refuses the out-of-range bind
// itself and the frame is merely wrong — "Invalid texture unit!", samplers
// reading stale unit-0 textures. Raised, the bind instead reaches a GL that
// clamps per-stage texture image units to 32 regardless and refuses it there,
// and the engine believes the unit was set, indexes its own unbounded GL state
// cache with it and writes through the function-pointer table every GL call
// dispatches through. The process then dies in an unrelated subsystem thousands
// of frames later, on device and in the viewer alike, branching through a slot
// holding whatever the page held. Stale unit-0 textures are the lesser fault.
//
// `target`, when set, narrows `use` to that one target: the offsets belong to
// the Unity build, but whether raising the cap is safe was shown per title
// (Beat Saber 1.40 is a 2022.3 build that dies when it is raised).
struct poke_cap_row { const char *ver; uint32_t singleton; uint32_t field; int use;
                      const char *target; };
static const struct poke_cap_row k_poke_caps[] = {
    // Unity 2021.3.45f2 — Shave & Stuff 1.12.3.2
    // (x7532973430077887, 2026-10-15). The base game runs after CPU skinning,
    // but dynamic hair colours and parts of the salon menu sample stale unit-0
    // textures while the engine reports "Invalid texture unit!" continuously.
    // Measured from this title's own libunity by the recipe above:
    //   0x1487c52  "OpenGL Error: Invalid texture unit!"
    //   0xb8fc34   bl 0x4b0fc4            ; the singleton getter
    //   0xb8fc38   ldr w8, [x0, #0xec]    ; the cap
    //   0xb8fc3c   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x4b0fc4   adrp x8, 0x18f4000 / ldr x0, [x8, #0xf40]
    // 0x18f4f40 is in the final RW LOAD's BSS (file-backed end 0x1892c78,
    // memory end 0x1982250). The 12-byte unit cache starts at 0x28 and its
    // active-unit field is at 0x330, leaving exactly 64 safe entries.
    { "2021.3.45f2", 0x18f4f40, 0xec, 1, "x7532973430077887" },

    // Space Channel 5 VR 1.0.5 / Unity 2021.3.24f1, measured from its ELF:
    // 7c06c0 queries MAX_COMBINED_TEXTURE_IMAGE_UNITS, then 7c06dc clamps
    // to 32 and 7c06e0 writes field ec. SetTexture at 7a3a14 rejects against
    // that field; getter 2932ec loads the singleton at 1393240.
    // Its 12-byte texture cache starts at 28 and ends before active-unit field
    // 330: 64 slots. Use the existing conservative 64 default, not all 96
    // ANGLE combined units. Additional instruction/count guards below keep
    // this store confined to this measured binary and the expected field.
    { "2021.3.24f1", 0x1393240, 0xec, 1, "x2249318828413747" },

    // Unity 2019.4.36f1 — Eye of the Temple 1.528 (2026-10-10). 1.28 million
    // "Invalid texture unit!" in 45 seconds at the cap of 32, and every texture
    // in the game black. Measured by the recipe above against this build's
    // libunity:
    //   0xdad5f4   "OpenGL Error: Invalid texture unit!"
    //   0x404730   bl 0x2e1bac            ; the singleton getter
    //   0x404734   ldr w8, [x0, #0xe8]    ; the cap
    //   0x404738   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x2e1bac   adrp x8, 0x112b000 / ldr x0, [x8, #0xec0]
    // 0x112bec0 lands in the RW LOAD's BSS (va 0x10729c0, filesz 0x5c568 <
    // memsz 0x15a6f0), a singleton pointer filled at runtime. Field 0xe8 as on
    // the other 2019 builds. BEFORE the "2019.4" row because the match is a
    // prefix test and takes the first hit; that row is Beat Saber's build.
    { "2019.4.36f1", 0x112bec0, 0xe8, 1, "x5361030930653377" },
    // Unity 2019.4.18f1 — Sniper Elite VR 1.03 (x2544573735586536, 2026-10-11).
    // ~640k "Invalid texture unit!" in a short session at the cap of 32: the
    // menu drew, but every level surface came out black and unlit (its
    // TexArray/MegaMaterial world shaders sample above unit 31). Measured by
    // the recipe above against this build's libunity:
    //   0x1231fe4  "OpenGL Error: Invalid texture unit!"
    //   0x6e5788   bl 0x53b558            ; the singleton getter
    //   0x6e578c   ldr w8, [x0, #0xe8]    ; the cap
    //   0x6e5790   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x53b558   adrp x8, 0x1684000 / ldr x0, [x8, #0xf40]
    // 0x1684f40 lands in the RW LOAD's BSS (va 0x15a8908, filesz 0x6f778 <
    // memsz 0x16cfe0), a singleton pointer filled at runtime. BEFORE the
    // "2019.4" row for the same prefix reason as the row above.
    { "2019.4.18f1", 0x1684f40, 0xe8, 1, "x2544573735586536" },
    // Unity 2019.4 — Beat Saber 1.28/1.6.0, the long-standing reference build.
    // Getter 0x313710 serves *(base + 0x122e340); cap at +0xe8.
    { "2019.4", 0x122e340, 0xe8, 0 },
    // Unity 2022.3 — Beat Saber 1.40 (2026-08-12). Measured by the recipe above:
    //   0x14b20b  "OpenGL Error: Invalid texture unit!"
    //   0xbd29b8  bl 0x6420f0            ; the singleton getter
    //   0xbd29bc  ldr w8, [x0, #0xec]    ; the cap
    //   0xbd29c0  cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x6420f0  adrp x8, 0x13d7000 / ldr x0, [x8, #0xcc0]
    // The version string is why this row is keyed on the real version and not
    // on the "2018.3.0a1" an earlier reading reported: see unity_version() —
    // that literal is present in this same libunity and is not its version.
    // Keyed on the FULL version rather than "2022.3": the match is a prefix
    // test, and VRChat is a different 2022.3 build with a different singleton,
    // so a "2022.3" key would aim this store at Beat Saber's address inside
    // VRChat's libunity. A minor version is not a build.
    { "2022.3.33f1", 0x13d7cc0, 0xec, 0 },
    // Unity 2022.3.22f2-DWR — VRChat (2026-08-13). Same recipe, and the guard
    // is INLINE in this build rather than behind a getter, so steps 3 and 4
    // collapse into one read:
    //   0x20ed3d  "OpenGL Error: Invalid texture unit!"
    //   0x142ef10 add x9, x9, #0xd3d     ; the only reference, in the reject block
    //   0x142ee28 b.ls 0x142eef4         ; the reject, taken when cap <= unit
    //   0x142ee18 adrp x24, 0x1994000    ; -+ the singleton, no bl in between
    //   0x142ee1c ldr  x26, [x24, #0x3c0]; -'
    //   0x142ee20 ldr  w8, [x26, #0xec]  ; the cap
    // 0x19943c0 lands in the RW LOAD's BSS (va 0x18c7970, filesz 0x1ca90 <
    // memsz 0x111676), which is what a singleton pointer filled at runtime
    // looks like.
    { "2022.3.22f2-DWR", 0x19943c0, 0xec, 1 },
    // Unity 2019.1.10f1 — Liminal (2026-08-23). The engine was rejecting units
    // >= 32 ~68k times a session, so its dialog/dynamic text sampled a stale
    // unit-0 texture and rendered as tofu boxes. Measured by the recipe above
    // against this build's libunity (the __klelf ELF inside the framework):
    //   0x10b04c0  "OpenGL Error: Invalid texture unit!"
    //   0xb0520c   bl 0x5bdaf8            ; the singleton getter
    //   0xb05210   ldr w8, [x0, #0xe8]    ; the cap
    //   0xb05214   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x5bdaf8   adrp x8, 0x149e000 / ldr x0, [x8, #0x280]
    // 0x149e280 lands in the RW LOAD (va 0x13b2000, memsz 0x1b3d18 -> 0x1565d18),
    // a singleton pointer filled at runtime. Field 0xe8 is the same slot 2019.4
    // uses; the singleton address is this build's own.
    { "2019.1.10f1", 0x149e280, 0xe8, 1 },
    // Unity 2022.3.62f3 — Keep Talking and Nobody Explodes 1.10.15 (2026-10-10).
    // ~21k "Invalid texture unit!" in one short session at the cap of 32; none
    // at boot once raised. Measured by the recipe above against the __klelf
    // ELF inside this build's libunity framework:
    //   0x120002   "OpenGL Error: Invalid texture unit!"
    //   0xa20418   bl 0x50e4b4            ; the singleton getter
    //   0xa2041c   ldr w8, [x0, #0xec]    ; the cap
    //   0xa20420   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x50e4b4   adrp x8, 0x10cd000 / ldr x0, [x8, #0x2c0]
    // 0x10cd2c0 lands in the last RW LOAD's BSS (va 0x104ef20, filesz 0x19180 <
    // memsz 0x103c60), a singleton pointer filled at runtime. Used on this
    // title only; another title on the same build is told it was declined.
    { "2022.3.62f3", 0x10cd2c0, 0xec, 1, "x2010043642376517" },
    // Unity 6000.3.22f1 — Premium Bowling 1.32.3 (x2773034772778845, 2026-10-12).
    // ~105k "Invalid texture unit!" in 26 seconds at the cap of 32, and every
    // glyph of its menus a filled box. Measured by the recipe above against
    // this build's libunity:
    //   0x169307   "OpenGL Error: Invalid texture unit!"
    //   0xea54c0   bl 0x71c184            ; the singleton getter
    //   0xea54c4   ldr w8, [x0, #0xec]    ; the cap
    //   0xea54c8   cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x71c184   adrp x8, 0x1796000 / ldr x0, [x8, #0xe40]
    // 0x1796e40 lands in .bss (va 0x1734d40, size 0x158f80), a singleton
    // pointer filled at runtime. Measured and NOT used: raised to 64, the run
    // faults within two frames, branching to 0 out of the bind at 0xec6df4.
    // This build's GL layer keeps its per-unit caches for 32 units (bound
    // texture at +0xffc, its target 0x80 further on, the bind dispatch table
    // right behind at +0x1178), and the cap is min(MAX_COMBINED, 32) at
    // 0xec1ee4 — 32 on a Quest as well. The units above 31 came from the
    // location answers instead; see KL_GLFB_BIND_PINS in kl_glfb.c.
    { "6000.3.22f1", 0x1796e40, 0xec, 0, "x2773034772778845" },
};
#define POKE_NROWS (sizeof k_poke_caps / sizeof k_poke_caps[0])

void kl_guest_poke_texture_unit_cap(void) {
    // Callable repeatedly, and the pump does exactly that. Unity does not treat
    // this field as immutable: it is set when the GfxDevice is built, so a
    // device rebuilt after we poked (an XR subsystem start, a graphics reset)
    // comes back at the default and the one-shot poke has silently expired.
    // That is invisible from outside, because the symptom is the ORIGINAL one —
    // "Invalid texture unit!" and samplers reading stale unit-0 textures —
    // arriving much later than boot, i.e. exactly when new content loads.
    //
    // The store itself is already idempotent (it re-reads the singleton, re-runs
    // the shape checks, and only writes when the field is BELOW what we want),
    // so all a re-check costs is a few loads. What had to change is the noise:
    // the SKIPPED paths describe a permanent condition and are said once.
    static int calls;
    const int first = (++calls == 1);
    const char *pv = getenv("KL_POKE_CAP");
    // The knobs are read HERE rather than at the call sites: a gate living in
    // one driver is a difference between the host and the headset with nothing
    // to report it. The raise is 64, matching the vendored ANGLE build
    // (kMaxShaderSamplers=32, combined 64), and is applied only to rows marked
    // `use`; KL_POKE_CAP=<n> sets the value and forces it on an unlisted or
    // unused row; KL_POKE_CAP_OFF=1 leaves the guest alone entirely.
    if (!pv && getenv("KL_POKE_CAP_OFF")) return;
    int poke_n = pv ? atoi(pv) : 64;
    if (poke_n <= 1) return;

    const char *ver = unity_version();
    const struct poke_cap_row *row = NULL;
    for (size_t i = 0; ver && i < POKE_NROWS; i++)
        if (strncmp(ver, k_poke_caps[i].ver, strlen(k_poke_caps[i].ver)) == 0) {
            row = &k_poke_caps[i];
            break;
        }
    // A row that exists and is not used declines by name, so the absence of a
    // raise on a build the offsets ARE known for never reads as an unlisted one.
    int use = row && row->use;
    if (use && row->target) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        use = t && strcmp(t, row->target) == 0;
    }
    if (row && !use && !pv) {
        if (first) fprintf(stderr, "  [poke] texture-unit cap: not raised on Unity %s — the "
                        "engine's own 32-unit check is what keeps an out-of-range unit out "
                        "of its GL state cache, and raising it trades stale unit-0 textures "
                        "for a later branch through a poisoned dispatch slot. "
                        "KL_POKE_CAP=<n> forces it.\n", row->ver);
        return;
    }
    // KL_POKE_CAP is also the override for an unlisted version: asking for it
    // explicitly is a statement that the offsets have been re-measured. It then
    // runs against the FIRST row's offsets, and the shape checks below are what
    // stop that from being a blind store.
    if (!row && !pv) {
        if (first) fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — no measured offsets "
                        "for Unity %s.\n"
                        "         Measure libunity's GfxDeviceGLES singleton/cap for "
                        "it (the recipe is above kl_guest_poke_texture_unit_cap), or set "
                        "KL_POKE_CAP=<n> to force.\n",
                ver ? ver : "an unknown version");
        return;
    }
    if (!row) row = &k_poke_caps[0];
    const uint32_t POKE_SINGLETON = row->singleton;
    const uint32_t POKE_FIELD     = row->field;

    kl_image *u = kl_find_image("libunity.so");
    if (!u) return;
    uint8_t *ub = (uint8_t *)kl_base(u);
    if (row->target && !strcmp(row->target, "x2249318828413747")) {
        static const uint32_t getter[] = {0x90008808, 0xf9412100, 0xd65f03c0};
        static const uint32_t limit[] = {0xb940ec08, 0x6b16011f, 0x54000629};
        if (kl_span(u) < 0x7a3a20 ||
            memcmp(ub + 0x2932ec, getter, sizeof getter) ||
            memcmp(ub + 0x7a3a14, limit, sizeof limit)) {
            if (first) fprintf(stderr, "  [poke] Space Channel texture cap: SKIPPED (binary guard mismatch)\n");
            return;
        }
        if (!pv) poke_n = 64;
        if (poke_n > 64) poke_n = 64; // the measured engine cache has 64 slots
    }
    // Shave & Stuff: the same two guards, from the instructions in its row's
    // comment (getter 0x4b0fc4, the cap check 0xb8fc38), and the same 64-slot
    // ceiling, which KL_POKE_CAP may lower but not raise.
    if (row->target && !strcmp(row->target, "x7532973430077887")) {
        static const uint32_t getter[] = {0x9000a228, 0xf947a100};
        static const uint32_t limit[] = {0xb940ec08, 0x6b16011f};
        if (kl_span(u) < 0xb8fc40 ||
            memcmp(ub + 0x4b0fc4, getter, sizeof getter) ||
            memcmp(ub + 0xb8fc38, limit, sizeof limit)) {
            if (first) fprintf(stderr, "  [poke] Shave & Stuff texture cap: SKIPPED (binary guard mismatch)\n");
            return;
        }
        if (poke_n > 64) poke_n = 64;
    }

    if ((size_t)POKE_SINGLETON + sizeof(void *) > kl_span(u)) {
        if (first) fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — libunity is %zu bytes, "
                        "shorter than the %#x offset.\n", kl_span(u), POKE_SINGLETON);
        return;
    }
    uint8_t *singleton = *(uint8_t **)(ub + POKE_SINGLETON);
    // Cheap shape checks behind the version gate: an aligned, mapped pointer
    // whose field reads as a plausible unit count. Any of these failing means
    // the offset no longer names what it used to, whatever the version says.
    if (!singleton || ((uintptr_t)singleton & 7) ||
        !addr_mapped(singleton + POKE_FIELD, sizeof(int32_t))) {
        if (first) fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — libunity+%#x holds %p, "
                        "which is not a mapped aligned object.\n",
                POKE_SINGLETON, (void *)singleton);
        return;
    }
    int32_t cur = *(int32_t *)(singleton + POKE_FIELD);
    if (row->target && !strcmp(row->target, "x2249318828413747") && cur != 32 && cur != 64) {
        if (first) fprintf(stderr, "  [poke] Space Channel texture cap: SKIPPED (unexpected count %d)\n", cur);
        return;
    }

    if (cur <= 0 || cur > 4096) {
        if (first) fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — +%#x reads %d, which is "
                        "not a texture-unit count.\n", POKE_FIELD, cur);
        return;
    }
    if (cur < poke_n) {
        // A poke after the first is not a second helping of the same fix — it
        // is the report that the guest RESET the field, which is a different
        // fact and the one worth reading.
        fprintf(stderr, "  [poke] texture-unit cap@+%#x %d -> %d%s\n",
                POKE_FIELD, cur, poke_n,
                first ? "" : "  (RESET by the guest since the last check —"
                             " the one-shot poke had expired)");
        *(int32_t *)(singleton + POKE_FIELD) = poke_n;
    }
}
