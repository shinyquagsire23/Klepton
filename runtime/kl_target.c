// See kl_target.h.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_jni.h"
#include "kl_target.h"

#define KL_TARGET_ROW(name, tree, apk, assets, libdir, entry, userdata, obb, kind) \
    { name, tree, apk, assets, libdir, entry, userdata, obb, kind },
static const kl_target TARGETS[] = {
#include "kl_target_table.h"
};
#undef KL_TARGET_ROW

const kl_target *kl_target_lookup(const char *name) {
    if (!name || !*name) return NULL;
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].name, name)) return &TARGETS[i];
    return NULL;
}

const kl_target *kl_target_default(void) {
    const kl_target *t = kl_target_lookup(KL_TARGET_DEFAULT_NAME);
    return t ? t : &TARGETS[0];
}

const char *kl_target_names(void) {
    static char buf[256];
    if (!*buf) {
        size_t n = 0;
        for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
            n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0,
                                  "%s%s", i ? ", " : "", TARGETS[i].name);
    }
    return buf;
}

// A tree with no table entry — the version-swap directories. Everything is
// derived from the tree name, so the libraries, the assets and the APK all come
// from the same place; the alternative (which is what the hardcoded defaults
// used to do) is to pair one version's libraries with another's assets, and
// Unity reports that three layers up as corrupt game data.
//
// The strings are strdup'd because the caller's argv outlives nothing in
// particular and this is read for the whole process.
static const kl_target *synthesize(const char *tree, const char *libdir) {
    static kl_target t;
    static char s_tree[512], s_apk[544], s_assets[544], s_libdir[1024], s_key[512];
    snprintf(s_tree,   sizeof s_tree,   "%s", tree);
    snprintf(s_apk,    sizeof s_apk,    "%s.apk", tree);
    snprintf(s_assets, sizeof s_assets, "%s/assets", tree);
    snprintf(s_libdir, sizeof s_libdir, "%s", libdir);

    // The userdata key: the tree with a trailing "-<digits>" removed, so
    // beatsaber-128 and beatsaber share one save/pairing profile. See the
    // header; KL_FILES_DIR overrides the whole directory.
    snprintf(s_key, sizeof s_key, "%s", tree);
    char *dash = strrchr(s_key, '-');
    if (dash && dash[1]) {
        int digits = 1;
        for (const char *p = dash + 1; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits) *dash = 0;
    }

    t = (kl_target){ .name = s_tree, .tree = s_tree, .apk = s_apk,
                     .assets = s_assets, .libdir = s_libdir,
                     .entry_lib = "libmain", .userdata = s_key,
                     // The version-swap trees are all Beat Saber, i.e. Unity, so
                     // the OBB layout is the one getObbDirs() answers. Stated
                     // rather than left NULL: every reader of this field builds
                     // a path out of it.
                     .obb = "obb",
                     .kind = KL_GUEST_UNITY };
    return &t;
}

const kl_target *kl_target_resolve(const char *arg) {
    if (!arg || !*arg) {
        const char *env = kl_env_str("KL_TARGET", NULL);
        if (env && *env) {
            const kl_target *t = kl_target_lookup(env);
            return t ? t : kl_target_default();   // a bad KL_TARGET is the
                                                  // caller's to report; do not
                                                  // silently run another guest
        }
        return kl_target_default();
    }
    if (!strchr(arg, '/')) return kl_target_lookup(arg);

    // A path. Its leading component is the tree; a table entry whose libdir or
    // tree matches wins, so `beatsaber/lib/arm64-v8a` — which is what every
    // gate in the Makefile passes — resolves to the real beatsaber target
    // rather than to a synthesized twin of it.
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].libdir, arg)) return &TARGETS[i];

    char tree[512];
    snprintf(tree, sizeof tree, "%s", arg);
    char *slash = strchr(tree, '/');
    if (slash) *slash = 0;
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].tree, tree)) return &TARGETS[i];

    return synthesize(tree, arg);
}

void kl_target_apply_host(const kl_target *t, const char *libdir_override) {
    if (!t) return;
    const char *libdir = libdir_override && *libdir_override ? libdir_override
                                                             : t->libdir;
    kl_set_library_path(libdir);
    kl_jni_set_native_lib_dir(libdir);
    kl_jni_set_assets_dir(t->assets);
    kl_jni_set_apk_path(t->apk);
    // Keyed on the guest rather than the APK — see kl_userdata_dir(), and the
    // synthesize() note above. KL_FILES_DIR still overrides it outright, which
    // is how a run gets a scratch profile.
    kl_jni_set_files_dir(kl_userdata_dir(t->userdata));
    // The raw "<apk>/assets/..." path map is derived from the assets
    // directory inside kl_jni's init, so nothing to do here.
}
