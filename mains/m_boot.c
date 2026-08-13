// M3 gate: enter the guest through its real entry point.
//
// The plan said this milestone ends at ANativeActivity_onCreate. It does not —
// there is no NativeActivity in this APK. AndroidManifest.xml declares
// com.unity3d.player.UnityPlayerActivity, and libmain.so exports exactly one
// thing: JNI_OnLoad. So the true entry is a JNI call, and the gate is that
// libmain's JNI_OnLoad runs against our synthetic JavaVM and registers the two
// natives the Java side expects.
//
// Phase 1 is the assertion. Phase 2 is reconnaissance: it drives the registered
// NativeLoader.load() to find out what the *next* milestone has to answer for,
// and runs in a forked child because an unimplemented JNI slot aborts by design.
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_glfb.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_egl.h"
#include "../runtime/kl_opensl.h"
#include "../runtime/kl_ovrp.h"
#include "../runtime/kl_ovrplat.h"
#include "../runtime/kl_view.h"
#include "../runtime/kl_sample.h"
#include "../runtime/kl_mprobe.h"
#include "../runtime/kl_il2cpp.h"
#include "../runtime/kl_fault.h"
#include "../runtime/kl_target.h"
#include "../tests/t_mtl_provider.h"

// Which guest, and where its libraries are. Both come from the target table
// (runtime/kl_target.h) rather than from four literals here and in kl_jni.c's
// defaults — the APK, the asset tree and the userdata directory have to agree
// with the libraries, and when they did not it was silent: SUPERHOT's libraries
// opening Beat Saber's APK is a guest reading someone else's game data.
//
//   ./build/m_boot                      the default target (beatsaber)
//   ./build/m_boot superhot             ...by name
//   ./build/m_boot superhot/lib/arm64-v8a   ...the same, by path
//
// The path form is what every command in CLAUDE.md and every Makefile gate
// passes, and it resolves to the same target as the name.
static const kl_target *TARGET;
static const char *LIBDIR;

typedef int  (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The fatal-signal reporter moved to runtime/kl_fault.c when the visionOS app
// needed the same thing inside a bundle (PLANNING §12.7) — it is a diagnostic
// that ships, not a harness detail. The sampling profiler stays host-only, so
// t_boot registers it rather than kl_fault.c referencing RUNTIME_DIAG.
static void install_fault_reporter(void) {
    kl_fault_add_reporter(kl_sample_stop_report);
    kl_fault_install();
}

// The entitlement refusal in kl_ovrplat.c is a policy guard, and Beat Saber does
// not currently reach it — the platform fails to initialise first, so the guard
// has never fired in a real run. A safety mechanism that has never been exercised
// is an assumption, not a mechanism, so it is checked here directly.
//
// Two properties matter: that an ownership query does not resolve to something
// callable-and-harmless, and that calling it dies rather than returning a value.
// The call is made in a child because passing the test means aborting.
// The resolve happens in the child too, not just the call. kl_ovrplat_sym records
// what was looked up, and the surface report separates entitlement lookups out
// specifically so a real one is visible — probing from the parent would leave our
// own test sitting in that list, indistinguishable from the guest asking.
// What does calling `name` actually do? Run it in a child, because for most of
// this surface the PASSING outcome is death. The value matters as well as the
// refusal now — the line below distinguishes the application's own entitlement
// (answered, and answered yes) from DLC (refused), and "it returned something"
// cannot tell those apart.
enum { KLDRM_UNRESOLVED = -1, KLDRM_ABORTED, KLDRM_ZERO, KLDRM_NONZERO };

static int drm_call(const char *name) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        // The refusal prints a paragraph and a surface report; not wanted here.
        freopen("/dev/null", "w", stderr);
        void *fn = kl_ovrplat_sym(name);
        if (!fn) _exit(2);              // resolved to nothing at all
        uint64_t v = ((uint64_t (*)(void))fn)();
        _exit(v ? 4 : 3);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) return KLDRM_ABORTED;
    if (!WIFEXITED(st)) return KLDRM_UNRESOLVED;
    switch (WEXITSTATUS(st)) {
    case 3: return KLDRM_ZERO;
    case 4: return KLDRM_NONZERO;
    default: return KLDRM_UNRESOLVED;
    }
}

// The line this asserts, in both directions. It moved once, deliberately, and
// the move is the reason the value is now checked rather than just the refusal:
//
//   The APPLICATION's own entitlement is answered, and answered yes. Klepton
//   runs an APK unpacked from a device the user owns, so ownership of the
//   application itself is the one thing in this family we can actually assert
//   — and it is asserted about a title the user already bought. It is not a
//   fabricated answer in the sense the refusals below are about; it is the
//   true state of affairs for the only way this project is used.
//
//   DLC is a different question and stays refused, because it is one we
//   genuinely cannot answer: paid content here is out-of-band asset data plus
//   a licence held by a platform that is absent, and neither half is on this
//   host to check. Claiming ownership of content we cannot see IS the
//   circumvention, so it keeps aborting by name.
//
// The two are separable because the API separates them — the viewer-entitled
// call is about the app, the AssetFile and IAP families are about content.
static int check_drm_guard(void) {
    // 1. The application's own entitlement: answered, and non-zero. Checked for
    //    its VALUE, since a 0 here reads to the guest as "not entitled" and
    //    sends a legitimately-owned title down its licence-failure path.
    int r = drm_call("ovr_Entitlement_GetIsViewerEntitled");
    if (r == KLDRM_UNRESOLVED)
        return fail("entitlement guard: the name resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("entitlement guard ABORTED — the application's own entitlement "
                    "is the one we can assert, and refusing it stops a title the "
                    "user owns");
    if (r != KLDRM_NONZERO)
        return fail("entitlement guard answered 0 — that reads as 'not entitled' "
                    "and is the licence-failure path, not a neutral answer");

    // 1b. ...and the COMPLETION says so, because the value alone stopped being
    //     the answer. It is a request, not a predicate: the SDK wraps the return
    //     in a `Request` and awaits a message, so a non-zero id with nothing
    //     arriving for it is a guest that waits forever — which is exactly what
    //     stopped 1.40 at the epilepsy screen, one request over. Assert the pair.
    {
        void *pop = kl_ovrplat_sym("ovr_PopMessage");
        void *ent = kl_ovrplat_sym("ovr_Entitlement_GetIsViewerEntitled");
        void *rid = kl_ovrplat_sym("ovr_Message_GetRequestID");
        void *err = kl_ovrplat_sym("ovr_Message_IsError");
        if (!pop || !ent || !rid || !err)
            return fail("entitlement guard: the message pump resolved to nothing");
        while (((void *(*)(void))pop)()) { }             // drain anything queued
        uint64_t req = ((uint64_t (*)(void))ent)();
        void    *msg = ((void *(*)(void))pop)();
        if (!msg)
            return fail("the entitlement request queued NO completion — the guest "
                        "awaits one and would wait forever");
        if (((uint64_t (*)(void *))rid)(msg) != req)
            return fail("the entitlement completion carries the wrong request id");
        if (((uint64_t (*)(void *))err)(msg))
            return fail("the entitlement completion is an ERROR — that is the "
                        "licence-failure path, not a neutral answer");
    }

    // 2. Anything that would DELIVER paid content must still die rather than
    //    answer. This is also what proves the ovr_AssetFile_GetList carve-out
    //    below is an exact name and not a widening of the "AssetFile" marker to
    //    the whole family.
    r = drm_call("ovr_AssetFile_DownloadById");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_AssetFile_DownloadById resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_AssetFile_DownloadById — a "
                    "content-delivery call must never be answered");

    // 3. The carve-out, in the other direction: enumerating installed DLC is
    //    answered with "none", because that grants nothing (kl_ovrplat.c,
    //    g_plat_absent). If this ever starts aborting the game stops at the
    //    language select again.
    r = drm_call("ovr_AssetFile_GetList");
    if (r == KLDRM_UNRESOLVED) return fail("ovr_AssetFile_GetList resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_AssetFile_GetList aborted — enumerating DLC we do not have "
                    "is a fact about this host, not a licence decision");

    printf("  the app's own entitlement answers yes AND completes, DLC delivery\n"
           "  refuses and aborts (guard verified); DLC enumeration answers\n"
           "  \"none\" without granting anything\n");
    return 0;
}

// ---- the guest run, in one function ----
// This is everything from libmain.so entry through the lifecycle recon. It
// runs either in the re-exec'd child (normal), or in-process (KL_NOFORK=1,
// for debugging). Returns only on a test failure; success is _exit(0), as
// before.
//
// view_pump is the KL_VIEW shape: the same sequence, but the frame pump runs
// until the viewer window closes instead of KL_FRAMES times, and the function
// RETURNS instead of _exit(0) — the guest is a spawned thread in that mode and
// the reports belong to the main thread after the join.
static volatile int g_view_quit;
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
static size_t unity_version_at(const char *b, size_t n, size_t i, int want_rev) {
    if (i + 10 >= n) return 0;
    if (b[i] != '2' || b[i + 1] != '0') return 0;
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
// One row per measured (Unity version prefix, singleton vaddr, cap field offset).
struct poke_cap_row { const char *ver; uint32_t singleton; uint32_t field; };
static const struct poke_cap_row k_poke_caps[] = {
    // Unity 2019.4 — Beat Saber 1.28/1.6.0, the long-standing reference build.
    // Getter 0x313710 serves *(base + 0x122e340); cap at +0xe8.
    { "2019.4", 0x122e340, 0xe8 },
    // Unity 2022.3 — Beat Saber 1.40 (2026-08-12). Measured by the recipe above:
    //   0x14b20b  "OpenGL Error: Invalid texture unit!"
    //   0xbd29b8  bl 0x6420f0            ; the singleton getter
    //   0xbd29bc  ldr w8, [x0, #0xec]    ; the cap
    //   0xbd29c0  cmp w8, w22 / b.ls     ; reject units >= cap
    //   0x6420f0  adrp x8, 0x13d7000 / ldr x0, [x8, #0xcc0]
    // The version string is why this row is keyed on 2022.3 and not on the
    // "2018.3.0a1" an earlier reading reported: see unity_version() — that
    // literal is present in this same libunity and is not its version.
    { "2022.3", 0x13d7cc0, 0xec },
};
#define POKE_NROWS (sizeof k_poke_caps / sizeof k_poke_caps[0])

static void poke_texture_unit_cap(void) {
    const char *pv = getenv("KL_POKE_CAP");
    int poke_n = pv ? atoi(pv) : 64;        // matches the vendored ANGLE rebuild
    if (poke_n <= 1) return;

    const char *ver = unity_version();
    const struct poke_cap_row *row = NULL;
    for (size_t i = 0; ver && i < POKE_NROWS; i++)
        if (strncmp(ver, k_poke_caps[i].ver, strlen(k_poke_caps[i].ver)) == 0) {
            row = &k_poke_caps[i];
            break;
        }
    // KL_POKE_CAP is also the override for an unlisted version: asking for it
    // explicitly is a statement that the offsets have been re-measured. It then
    // runs against the FIRST row's offsets, and the shape checks below are what
    // stop that from being a blind store.
    if (!row && !pv) {
        fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — no measured offsets "
                        "for Unity %s.\n"
                        "         Measure libunity's GfxDeviceGLES singleton/cap for "
                        "it (the recipe is above poke_texture_unit_cap), or set "
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
    if ((size_t)POKE_SINGLETON + sizeof(void *) > kl_span(u)) {
        fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — libunity is %zu bytes, "
                        "shorter than the %#x offset.\n", kl_span(u), POKE_SINGLETON);
        return;
    }
    uint8_t *singleton = *(uint8_t **)(ub + POKE_SINGLETON);
    // Cheap shape checks behind the version gate: an aligned, mapped pointer
    // whose field reads as a plausible unit count. Any of these failing means
    // the offset no longer names what it used to, whatever the version says.
    if (!singleton || ((uintptr_t)singleton & 7) ||
        !addr_mapped(singleton + POKE_FIELD, sizeof(int32_t))) {
        fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — libunity+%#x holds %p, "
                        "which is not a mapped aligned object.\n",
                POKE_SINGLETON, (void *)singleton);
        return;
    }
    int32_t cur = *(int32_t *)(singleton + POKE_FIELD);
    if (cur <= 0 || cur > 4096) {
        fprintf(stderr, "  [poke] texture-unit cap: SKIPPED — +%#x reads %d, which is "
                        "not a texture-unit count.\n", POKE_FIELD, cur);
        return;
    }
    if (cur < poke_n) {
        fprintf(stderr, "  [poke] texture-unit cap@+%#x %d -> %d\n",
                POKE_FIELD, cur, poke_n);
        *(int32_t *)(singleton + POKE_FIELD) = poke_n;
    }
}

static int recon_run(int view_pump) {
    install_fault_reporter();
    // Strict: an unimplemented *call* is fatal. Lookups are not, so this
    // stops only where the surface genuinely ends. KL_PERMISSIVE=1 flips it
    // to a zero return, which collects a whole batch in one run when pushing
    // into new territory — scouting only, since the guest then carries on
    // with answers we made up.
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);

    // Armed before anything runs: texture uploads happen all through init and the
    // lifecycle, not just inside the frame pump.
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    char path[1024];
    snprintf(path, sizeof path, "%s/libmain.so", LIBDIR);

    printf("=== libmain.so entry ===\n");
    kl_image *main_img = kl_load_auto(path);
    if (!main_img) return fail(kl_error());
    kl_register_image("libmain.so", main_img);
    kl_run_init(main_img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(main_img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    int version;
    kl_jni_local_frame_push();      // the JVM would pop each native's local
    version = onload(kl_jni_vm(), NULL);   // frame on return; the host plays
    kl_jni_local_frame_pop();       // that half (see kl_jni.h)
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    // libmain registers com.unity3d.player.NativeLoader.{load,unload} — the
    // shim Unity's Java side calls to dlopen libunity.so.
    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load   = kl_jni_native(CLS, "load", NULL);
    void *unload = kl_jni_native(CLS, "unload", NULL);
    if (!load || !unload) return fail("NativeLoader natives were not registered");
    printf("  registered %s.load=%p unload=%p\n", CLS, load, unload);

    printf("\n=== M3 EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");

    // ---- phase 2: reconnaissance, non-fatal ----
    printf("\n=== recon: driving NativeLoader.load(\"libunity.so\") ===\n");
    fflush(NULL);
    // load() takes the *directory* — it appends "/libunity.so" itself.
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(LIBDIR));
    kl_jni_local_frame_pop();
    printf("  NativeLoader.load returned %d\n", ok);

    // UnityPlayer's constructor calls initJni(Context) first (UnityPlayer.smali
    // line 372). It is `private final native`, so an instance method: the guest
    // sees (JNIEnv*, jobject thiz, jobject context). Both objects are opaque to
    // us — what matters is what libunity asks them for.
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (initJni) {
        printf("\n=== recon: UnityPlayer.initJni(Context) ===\n");
        fflush(NULL);
        void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
        // On device the Context is the Activity — AndroidManifest.xml declares
        // UnityPlayerActivity — and Unity checks that with IsInstanceOf. Handing
        // it a bare Context would send it down the no-Activity path.
        // The shared singleton, not a fresh instance: Unity also reads this
        // back through the static UnityPlayer.currentActivity and compares.
        void *context = kl_jni_activity();
        kl_jni_local_frame_push();
        ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, context);
        kl_jni_local_frame_pop();
        printf("  initJni returned\n");
    }

    // Lifecycle, in the order UnityPlayerActivity drives it: attach a
    // surface, resume, then pump one frame. This is where M4 runs into M5 —
    // nativeRecreateGfxState is what reaches for EGL.
    if (getenv("KL_LIFECYCLE")) {
        // P5.3: with KL_GLFB_MTL=1, the eye textures Unity is about to ask for
        // become MTLTextures we allocated — the host stand-in for what
        // Compositor Services will hand over. Registered here because
        // ovrp_SetupEyeTexture2 arrives inside nativeRecreateGfxState, below.
        kl_mtl_provider_install();
        void *surface = kl_jni_new_object("android/view/Surface");
        void *thiz2   = kl_jni_new_object("com/unity3d/player/UnityPlayer");
        struct { const char *name; int kind; } seq[] = {
            {"nativeRecreateGfxState", 2}, {"nativeResume", 0}, {"nativeRender", 1},
        };
        for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", seq[i].name, NULL);
            if (!fn) { printf("  %s: not registered\n", seq[i].name); continue; }
            printf("\n=== recon: UnityPlayer.%s ===\n", seq[i].name);
            fflush(NULL);
            // The render loop may block; do not hang the sweep. KL_ALARM
            // widens the window when the question is what it is waiting on.
            const char *aenv = getenv("KL_ALARM");
            alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : 20);
            kl_jni_local_frame_push();
            if (seq[i].kind == 2)
                ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), thiz2, 0, surface);
            else if (seq[i].kind == 1)
                printf("  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz2));
            else
                ((void (*)(void *, void *))fn)(kl_jni_env(), thiz2);
            kl_jni_local_frame_pop();
            alarm(0);
            printf("  %s returned\n", seq[i].name);
            // Android's UI thread runs its looper between callbacks; here
            // the host has to pump it, or the queue only ever grows.
            alarm(10);
            unsigned ran = kl_jni_drain_ui_tasks();
            alarm(0);
            if (ran) printf("  drained %u posted task%s\n", ran, ran == 1 ? "" : "s");
        }

        // One nativeRender is the engine's first frame and it is almost all
        // setup — no scene is loaded and nothing is drawn yet, which is why
        // the GL surface looked so small. KL_FRAMES pumps the render loop
        // the way UnityPlayer's own thread would, draining the posted-task
        // queue between frames as the UI thread's looper does.
        // In view_pump mode (KL_VIEW) the pump instead runs until the viewer
        // window closes, paced to 72 Hz — the Quest 2 display frequency we
        // report through ovrp_GetSystemDisplayFrequency, so the Choreographer
        // ticks at the rate the engine believes the hardware runs at. The
        // watchdog alarm is not armed there: the window may stay open for
        // minutes, and a human at the keyboard IS the watchdog.
        const char *fenv = getenv("KL_FRAMES");
        unsigned frames = fenv ? (unsigned)strtoul(fenv, NULL, 10) : 0;
        if (frames || view_pump) {
            void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", "nativeRender", NULL);
            // Default is poke 64, matching the vendored ANGLE rebuild
            // (kMaxShaderSamplers=32, combined 64); KL_POKE_CAP=<n> overrides
            // and forces it on an unlisted Unity version, KL_POKE_CAP_OFF=1
            // leaves it alone entirely.
            if (getenv("KL_POKE_CAP") || !getenv("KL_POKE_CAP_OFF"))
                poke_texture_unit_cap();
            if (view_pump)
                printf("\n=== recon: pumping frames until the viewer closes ===\n");
            else
                printf("\n=== recon: pumping %u frames ===\n", frames);
            fflush(NULL);
            const char *aenv2 = getenv("KL_ALARM");
            unsigned budget = aenv2 ? (unsigned)strtoul(aenv2, NULL, 10) : 60;
            if (!view_pump) alarm(budget);
            // KL_SAMPLE_MS: while the pump runs, sample every guest thread's
            // pc/backtrace and resolve it (kl_sample.c) — built to name the
            // loop the loading-pace arc kept measuring. The metadata sits next
            // to the libs in the unpacked APK tree.
            const char *senv = getenv("KL_SAMPLE_MS");
            int sampling = 0;
            if (senv) {
                char meta[1024];
                const char *menv = getenv("KL_IL2CPP_METADATA");
                if (menv) {
                    snprintf(meta, sizeof meta, "%s", menv);
                } else {
                    // <apk>/lib/arm64-v8a -> <apk>/assets/bin/Data/Managed/...
                    snprintf(meta, sizeof meta, "%s", LIBDIR);
                    char *tail = strstr(meta, "/lib/");
                    if (tail) *tail = 0;
                    snprintf(meta + strlen(meta), sizeof meta - strlen(meta),
                             "/assets/bin/Data/Managed/Metadata/global-metadata.dat");
                }
                sampling = kl_sample_start(
                    (unsigned)strtoul(senv, NULL, 10), meta);
            }
            const long frame_ns = 1000000000L / 72;
            unsigned haptic_pulses = 0;
            unsigned i;
            for (i = 0; fn && (view_pump ? !g_view_quit : i < frames); i++) {
                struct timespec t0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                // Pin this frame's poses first, so every ovrp_GetNodePoseState
                // inside the frame answers the same thing and the pose recorded
                // for timewarp is the one the picture was drawn from. On the
                // host the frontend and the guest are usually the same thread,
                // so this changes nothing — but the viewer is what proves the
                // reprojection math before a device is available, and it can
                // only prove it if it runs the same shape.
                kl_ovrp_frame_latch();
                // The frame clock next: on Android the Choreographer's
                // doFrame is what wakes the engine for a frame, and
                // nativeRender then draws what it decided. Ticking after
                // rendering would hand every frame the previous one's time.
                // The frame clock is now a free-running host thread started at
                // the first Choreographer postFrameCallback — it is NOT driven
                // from here. Doing it inline is how the pump blocked forever: a
                // doFrame could only be delivered before nativeRender, and
                // nativeRender waits on the refresh counter a doFrame advances.
                // One local frame per pumped frame, as the JVM gives each
                // nativeRender on Android — the guest's per-frame locals are
                // meant to die here (see kl_jni.h).
                kl_jni_local_frame_push();
                ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz2);
                kl_jni_local_frame_pop();
                kl_jni_drain_ui_tasks();
                // The haptics seam's host end. Nothing on macOS can vibrate, so
                // this only drains and counts — but draining is the point: it
                // is what makes the host a working A/B for the whole path down
                // to the actuator, and KL_HAPTICS_TRACE=1 then prints the same
                // pulses a headset would have played. (The queue does not need
                // it to stay healthy; it retires samples on the wall clock
                // whether anyone is listening or not.)
                for (int hand = 0; hand < 2; hand++) {
                    float amp, secs;
                    if (kl_ovrp_haptics_pull(hand, &amp, &secs)) haptic_pulses++;
                }
                // KL_PROBE_INPUT: ask Unity's own managed API what it sees —
                // joystick count, the bound axes, the XR node poses. Between
                // frames on the thread that just ran one, which is where
                // managed calls are safe.
                kl_mprobe_tick(i);
                if (view_pump) {
                    struct timespec t1;
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long used = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                                (t1.tv_nsec - t0.tv_nsec);
                    if (used < frame_ns) {
                        struct timespec rem = { 0, frame_ns - used };
                        nanosleep(&rem, NULL);
                    }
                }
            }
            alarm(0);
            if (sampling) kl_sample_stop_report(stdout);
            printf("  pumped %u frames\n", i);
            if (haptic_pulses)
                printf("  haptic pulses drained: %u (nothing to play them on "
                       "here — see kl_ovrp.h)\n", haptic_pulses);
            // P5.3's gate. Not "did the interop bind" — that is reported when it
            // happens — but "did the guest's rendering arrive in the MTLTexture".
            // The lit count uses the same luma threshold as kl_glfb's capture, so
            // it is directly comparable with the reference path's number: the two
            // should agree, and a lit reference frame beside a black interop one
            // means the binding took and the rendering went elsewhere.
            // ...and it fires for an eye MTLTexture that arrived any way at
            // all, not just through a registered provider. On the VULKAN path
            // there IS no provider: MoltenVK backs the guest's eye VkImage with
            // a texture of its own and kl_vulkan.c publishes that one
            // (kl_glfb_note_eye_mtl_texture), so a provider test would have
            // skipped the only guest whose compositor wiring is new.
            if (kl_glfb_has_mtl_provider() || kl_glfb_eye_mtl_texture(0, 0, NULL)) {
                printf("\n=== P5 interop: the guest's frame, read back through Metal ===\n");
                // Under the viewer's hardware compositor there IS no reference:
                // registering a GPU fence replaces the readback, so kl_glfb has
                // counted nothing and a bare 0 here would read as a black frame.
                if (!kl_glfb_has_mtl_provider())
                    printf("  reference: KL_VK_OUT — on the Vulkan path the eye "
                           "is read back from the VkImage, and this is the SAME "
                           "storage seen as Metal\n");
                else if (kl_glfb_has_gpu_fence())
                    printf("  reference: none — the GPU compositor is driving, "
                           "so nothing was read back\n");
                else
                    printf("  reference (glReadPixels, kl_glfb): %lu lit\n",
                           kl_glfb_last_frame_lit());
                for (int eye = 0; eye < 2; eye++) {
                    int w = 0, h = 0;
                    unsigned long lit = kl_mtl_count_lit(eye, 0, &w, &h);
                    unsigned long n = w && h ? ((unsigned long)((w + 7) / 8)
                                              * (unsigned long)((h + 7) / 8)) : 0;
                    printf("  eye %d MTLTexture %dx%d: %lu/%lu lit, mean luma %u%s\n",
                           eye, w, h, lit, n, kl_mtl_mean_luma(),
                           lit ? "" : "  <<< BLACK");
                    // A picture, not just a count: KL_GLFB_OUT already holds the
                    // reference frames, so writing the interop eyes beside them
                    // is what makes "does it render the same" answerable.
                    const char *out = getenv("KL_GLFB_OUT");
                    if (out) {
                        char p[1200];
                        snprintf(p, sizeof p, "%s/mtl_eye%d.png", out, eye);
                        printf("  eye %d -> %s%s\n", eye, p,
                               kl_mtl_dump_png(eye, 0, p) ? "" : "  (write FAILED)");
                    }
                }
            }
            const char *sd = getenv("KL_DUMP_SHADERS");
            if (sd) kl_egl_dump_shaders(sd);
            if (getenv("KL_DUMP_TEXTURES"))
                printf("  wrote %u texture upload%s as PNG\n",
                       kl_egl_texture_count(),
                       kl_egl_texture_count() == 1 ? "" : "s");
        }
    }

    if (view_pump)
        return 0;   // the main thread prints the reports after the join
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    kl_ovrp_report(stdout);
    kl_ovrplat_report(stdout);
    fflush(NULL);   // _exit does not flush stdio, and the report is the point
    _exit(0);
}

// The KL_VIEW guest thread: the same recon sequence the re-exec'd child runs,
// but in-process and pumping until the window closes. kl_thread_init() first —
// this thread runs guest code, and guest code needs its TLS slot (trap 1).
static void *view_guest_thread(void *arg) {
    (void)arg;
    kl_thread_init();
    recon_run(1);
    return NULL;
}

// KL_VIEW=1: the interactive frontend (kl_view.c). This deliberately skips
// BOTH process games main() normally plays:
//  - the DRM-guard fork test, because the guard itself lives in kl_ovrplat.c
//    and is unaffected by who forks, and
//  - the re-exec, because Metal's XPC shader compiler refuses forked children
//    (the AGX abort story above) and a windowed app never forks in the first
//    place — in-process is the whole point.
// The guest runs on a spawned thread; the main thread runs SDL, because macOS
// requires windowing on the main thread.
static int view_run(void) {
    // kl_env_on, not getenv: this has to agree with kl_glfb_enabled() about what
    // KL_GLFB=0 means, or the viewer starts on the strength of a knob the
    // renderer read as off and then displays nothing.
    //
    // ...unless the guest's API is Vulkan, and that is not knowable here: it is
    // measured by kl_vulkan_guest_active() long after this point, from what the
    // guest actually did. A Vulkan guest never brings ANGLE up at all, and its
    // eye textures reach the compositor from MoltenVK instead
    // (kl_glfb_note_eye_mtl_texture), so KL_GLFB has nothing to do with whether
    // there is a picture. Refusing here would refuse the one target whose
    // compositing is new. So this is a NOTE now, not a refusal — the viewer
    // waits for an eye texture from whichever source produces one, and says so
    // in its HUD if none ever arrives.
    if (!kl_env_on("KL_GLFB", 0))
        fprintf(stderr, "view: KL_GLFB is not set — nothing will be displayed "
                        "unless the guest turns out to render through VULKAN, "
                        "whose eye textures do not come from ANGLE\n");
    if (!kl_view_available()) {
        fprintf(stderr, "KL_VIEW=1 but t_boot was built without SDL3\n");
        return 1;
    }
    // The frame-out seam, and which of its two implementations to use. Decided
    // HERE, on the main thread, before the guest thread exists: kl_glfb_mtl_
    // device() brings ANGLE up to answer, and kl_glfb_init() is not something
    // two threads may race into.
    //
    // Hardware is the default — the guest's eye textures become MTLTextures we
    // allocated and the viewer composites one straight into the window's
    // CAMetalLayer, so no frame is ever read back or copied. KL_VIEW_CPU=1
    // keeps the old glReadPixels path, which is the A/B when the compositor
    // shows the wrong picture.
    int hw = 0;
    if (getenv("KL_VIEW_CPU")) {
        fprintf(stderr, "view: KL_VIEW_CPU=1 — readback path\n");
    } else if (!kl_env_on("KL_GLFB", 0)) {
        // No GL renderer was asked for, so there is no readback to fall back
        // to and nothing to bring ANGLE up for. The compositor path is the only
        // one that can show anything, and it finds the guest's eye texture by
        // itself whichever API produced it — which for a Vulkan guest is the
        // only way a picture exists at all.
        hw = 1;
        fprintf(stderr, "view: no GL renderer — the compositor will wait for an "
                        "eye texture (Vulkan)\n");
    } else if (!kl_glfb_mtl_device()) {
        fprintf(stderr, "view: no MTLDevice from ANGLE — readback path\n");
    } else {
        kl_mtl_provider_install();          // installs unconditionally under KL_VIEW
        hw = kl_glfb_has_mtl_provider();
    }
    if (!hw) kl_glfb_set_frame_sink(kl_view_frame_sink, NULL);
    pthread_t guest;
    if (pthread_create(&guest, NULL, view_guest_thread, NULL)) {
        fprintf(stderr, "view: pthread_create failed\n");
        return 1;
    }
    int rc = kl_view_main(LIBDIR, hw);   // returns when the window closes
    g_view_quit = 1;
    pthread_join(guest, NULL);
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    kl_ovrp_report(stdout);
    kl_ovrplat_report(stdout);
    return rc;
}

int main(int argc, char **argv) {
    TARGET = kl_target_resolve(argc > 1 ? argv[1] : NULL);
    if (!TARGET) {
        fprintf(stderr, "FAIL: unknown target '%s' — one of: %s "
                        "(or a path to a guest lib directory)\n",
                argv[1], kl_target_names());
        return 1;
    }
    LIBDIR = TARGET->libdir;
    // Everything the guest is told about itself, from one row: the library
    // path, the assets, the APK it opens as a zip, and the userdata directory
    // its saves land in.
    kl_target_apply_host(TARGET, NULL);
    printf("=== target: %s (%s, %s) ===\n", TARGET->name, LIBDIR, TARGET->apk);

    // Re-entry of the re-exec'd recon child (see below).
    if (getenv("KL_RECON_CHILD"))
        return recon_run(0);

    if (getenv("KL_VIEW"))
        return view_run();

    printf("=== DRM policy guard ===\n");
    // Under a debugger the forked child inherits the parent's Mach exception
    // ports, so its abort is intercepted and never reads as a SIGABRT to
    // waitpid. KL_SKIP_GUARD_TEST skips this self-test when running under
    // lldb — the guard itself in kl_ovrplat.c is unaffected.
    if (!getenv("KL_SKIP_GUARD_TEST") && check_drm_guard()) return 1;

    // ---- phase 2: reconnaissance, in a child ----
    // Unimplemented JNI slots abort the process on purpose, so this runs in a
    // child. Whatever it prints before dying is the M4 work list.
    //
    // The child is re-EXEC'd, not a bare fork. Metal's shader compiler is an
    // XPC service that refuses forked children, and AGX treats the resulting
    // cold-compile failure of one of its OWN internal blit shaders as fatal
    // (the long-standing "AGX abort"): the failure only showed once the driver
    // needed a compile — a blit in texture setup — and only ever in the child,
    // which is why every standalone recipe was clean. exec resets the XPC
    // state, so the re-exec'd child compiles like any other process.
    // KL_NOFORK=1 skips the child entirely: macOS lldb does not follow exec
    // any better than fork, so debugging the guest needs the guest threads in
    // the traced process, and the debugger becomes the reporter.
    printf("\n=== recon: spawning child ===\n");
    fflush(NULL);
    if (getenv("KL_NOFORK"))
        return recon_run(0);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("KL_RECON_CHILD", "1", 1);
        execl(argv[0], argv[0], LIBDIR, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        // How it stopped is the first question every time: an unimplemented JNI
        // slot aborts (SIGABRT after the report), SIGALRM means the guest
        // blocked, and anything else is a real fault in guest code.
        if (WIFSIGNALED(st))
            printf("\n  (recon stopped on signal %d — %s)\n", WTERMSIG(st),
                   strsignal(WTERMSIG(st)));
        else
            printf("\n  (recon child exited %d)\n", WEXITSTATUS(st));
        printf("  (see the JNI surface report above)\n");
        return fail("guest init did not complete: an unimplemented JNI call was reached");
    }
    printf("\n=== M4 (partial): initJni completed with no unimplemented JNI calls ===\n");
    return 0;
}
