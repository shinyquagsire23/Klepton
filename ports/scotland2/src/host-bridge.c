#include "kl_scotland2.h"
#include "klepton.h"
#include "kl_driver.h"
#include "kl_jni.h"
#include "kl_aot_hooks.h"
#include "apple-platform.h"
#include "kl_extension.h"
#include "kl_unicode.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef SC2_DIAGNOSTICS
#define SC2_DIAGNOSTICS 1
#endif
static int diagnostic(void) { return SC2_DIAGNOSTICS && kl_env_on("KL_SCOTLAND2_DIAGNOSTICS",1); }
static char failure[512];
static int started, init_finished;
static void *native_core;
static uintptr_t abort_dispatch=(uintptr_t)abort;
static int native_abort_registered;
__attribute__((noreturn)) static void native_abort_gate(void) {
    void (*dispatch)(void)=(void *)__atomic_load_n(&abort_dispatch,__ATOMIC_ACQUIRE);
    dispatch();
    abort(); // Preserve abort's contract even if a diagnostic hook returns.
}
static _Thread_local unsigned module_open_depth;
int kl_scotland2_strict_load(void) { return module_open_depth!=0; }
void *kl_scotland2_open_module(const char *path,int flags) {
    void *(*open)(const char*,int)=kl_shim_lookup("dlopen");
    const char *(*error)(void)=kl_shim_lookup("dlerror");
    (void)error();
    ++module_open_depth;
    void *result=open(path,flags);
    --module_open_depth;
    return result;
}
static kl_image *android_paths;
static kl_image *android_flamingo,*android_flamingo_probe;
static char customtypes_probe_path[4096];
static char lapiz_profile_path[4096], bsml_profile_path[4096], bsml_probe_path[4096];
static char songcore_profile_path[4096],songcore_probe_path[4096];
static char metacore_profile_path[4096],metacore_probe_path[4096];
static char playlistcore_profile_path[4096];
static int songdownloader_probe;
static char remaining_profile_root[4096];
#if SC2_DIAGNOSTICS
static int verify_mod_hooks(const char *,const char *,unsigned);
#endif
static const int bridge_handle;
static int (*after_init)(void);
static int (*before_scene)(void);
static int (*set_handles)(void *, void *);
static const char *(*core_error)(void);
// Beat Saber 1.40 ships Unity 2022.3.33f1: il2cpp_init has a void API.
static void (*real_init)(const char *);
static void *(*domain_get)(void);
static char paper_logs[2][4096];
static const char paper_marker[] = "SC2_PAPER_SIGNED_RUNTIME_20261023";
static void *(*domain_original)(void), *(*domain_previous)(void);
static unsigned domain_first_calls, domain_second_calls;
static void *domain_first(void) {
    __atomic_add_fetch(&domain_first_calls,1,__ATOMIC_RELAXED);
    return domain_original();
}
static void *domain_second(void) {
    __atomic_add_fetch(&domain_second_calls,1,__ATOMIC_RELAXED);
    return domain_previous();
}

static int owns(void *handle, const void *symbol, const char *path) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    const char *owner = kl_addr_image(symbol, NULL);
    return handle == kl_find_image(base) && owner && !strcmp(owner, base);
}
static int stop(const char *message) {
    snprintf(failure, sizeof failure, "%s", message ? message : "unknown loader failure");
    fprintf(stderr, "[scotland2] STOP %s\n", failure);
    return -1;
}
const char *kl_scotland2_error(void) { return failure; }

void *kl_scotland2_sym(const char *name) {
    if (native_abort_registered && name && !strcmp(name,"abort")) return native_abort_gate;
    if (!native_core || !name) return NULL;
    if (!strcmp(name,"sc2_unicode_draw_glyph")) return kl_unicode_draw_glyph;
    if (!strcmp(name,"sc2_aot_exchange")) return kl_aot_exchange;
    if (!strcmp(name,"sc2_aot_error_code")) return kl_aot_error_code;
    if (!strcmp(name,"sc2_aot_original")) return kl_aot_original;
    if (!strcmp(name,"sc2_aot_restore")) return kl_aot_restore;
    if (!strcmp(name,"sc2_aot_entry_word")) return kl_aot_entry_word;
    if (!strcmp(name,"sc2_aot_current")) return kl_aot_current;
    if (!strcmp(name,"sc2_aot_begin")) return kl_aot_begin;
    if (!strcmp(name,"sc2_aot_end")) return kl_aot_end;
    if (!strcmp(name,"sc2_aot_copy_code")) return kl_aot_copy_code;
    static const char *const api[] = {
        "modloader_get_failed", "modloader_get_path", "modloader_get_source_path",
        "modloader_get_root_load_path", "modloader_get_files_dir", "modloader_get_external_dir",
        "modloader_get_libil2cpp_path", "modloader_get_application_id", "modloader_get_mod",
        "modloader_force_unload", "modloader_get_loaded", "modloader_get_all",
        "modloader_free_results", "modloader_require_mod", "modloader_add_ld_library_path",
        "modloader_jvm", "modloader_libil2cpp_handle", "modloader_unity_handle",
        "libs_opened", "early_mods_opened", "late_mods_opened", "current_load_phase",
    };
    for (size_t i = 0; i < sizeof api / sizeof *api; ++i)
        if (!strcmp(name, api[i])) return dlsym(native_core, name);
    // These implementations create real Android C++ objects in a guest DSO.
    static const char *const cpp_paths[] = {
        "_ZN9modloader18get_modloader_pathEv", "_ZN9modloader25get_modloader_source_pathEv",
        "_ZN9modloader28get_modloader_root_load_pathEv", "_ZN9modloader13get_files_dirEv",
        "_ZN9modloader16get_external_dirEv", "_ZN9modloader18get_libil2cpp_pathEv",
        "_ZN9modloader18get_application_idEv",
    };
    if (android_paths) for (size_t i = 0; i < sizeof cpp_paths / sizeof *cpp_paths; ++i)
        if (!strcmp(name, cpp_paths[i])) return kl_sym(android_paths, name);
    // Original Capstone C exports, when explicitly linked into the guest bridge.
    if (android_paths && !strncmp(name, "cs_", 3)) return kl_sym(android_paths, name);
    if (android_flamingo && (!strncmp(name,"_ZN8flamingo",12) || !strncmp(name,"_ZNK8flamingo",13) || !strncmp(name,"flamingo_",9)))
        return kl_sym(android_flamingo,name);
    return NULL;
}
int kl_scotland2_claims(const char *path) {
    if (!native_core || !path) return 0;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    return !strcmp(base, "libsl2.so");
}
void *kl_scotland2_dlopen(const char *path) {
    return kl_scotland2_claims(path) ? (void *)&bridge_handle : NULL;
}
int kl_scotland2_is_handle(void *handle) { return handle == (void *)&bridge_handle; }

static int paper_log_contains(const char *path) {
    char text[16384];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    // Startup output can exceed one buffer before the marker is queued.
    // Keep the overlap so a marker crossing a read boundary is also found.
    size_t keep = 0, size;
    while ((size = fread(text + keep, 1, sizeof text - 1 - keep, file)) > 0) {
        size += keep; text[size] = 0;
        if (strstr(text, paper_marker)) { fclose(file); return 1; }
        keep = strlen(paper_marker) - 1;
        if (keep > size) keep = size;
        memmove(text, text + size - keep, keep);
    }
    fclose(file);
    return 0;
}
static void *check_paper_logs(void *unused) {
    (void)unused;
    for (int i = 0; i < 200; ++i) {
        if (paper_log_contains(paper_logs[0]) && paper_log_contains(paper_logs[1])) {
            fprintf(stderr, "[scotland2] PAPER_LOG_OK actual Paper wrote main and context log files\n");
            return NULL;
        }
        usleep(50000);
    }
    fprintf(stderr, "[scotland2] PAPER_LOG_FAILED expected message absent from main/context files after 10 seconds\n");
    return NULL;
}
static int exercise_paper(const char *files) {
    kl_image *paper = kl_find_image("libpaper2_scotland2.so");
    if (!paper) return 0; // The original tinyxml2-only test remains supported.
    const kl_stats *stats = kl_get_stats(paper);
    if (stats->imports_missing) return stop("Paper has unresolved imports");
    bool (*inited)(void) = kl_sym(paper, "paper2_get_inited");
    void (*context)(const char *) = kl_sym(paper, "paper2_register_context_id");
    bool (*queue)(int,const char *,const char *,const char *,int,int,const char *) = kl_sym(paper, "paper2_queue_log_ffi");
    if (!inited || !context || !queue || !inited()) return stop("Paper constructor or C API unavailable");
    const char *names[] = {"Paperlog.log", "Scotland2PortTest.log"};
    for (int i = 0; i < 2; ++i) {
        int size = snprintf(paper_logs[i], sizeof paper_logs[i], "%s/ModData/com.beatgames.beatsaber/logs2/%s", files, names[i]);
        if (size < 0 || (size_t)size >= sizeof paper_logs[i]) return stop("Paper log path too long");
    }
    context("Scotland2PortTest");
    // LogLevel::Info == 0 in Paper's published C bindings. No C++ ABI crossing.
    if (!queue(0,"Scotland2PortTest",paper_marker,__FILE__,__LINE__,1,"exercise_paper")) return stop("Paper rejected log message");
    pthread_t observer;
    if (pthread_create(&observer, NULL, check_paper_logs, NULL)) return stop("could not start Paper log observer");
    pthread_detach(observer);
    fprintf(stderr, "[scotland2] PAPER_READY %u imports resolved; constructor and real log queue succeeded\n", stats->imports_bound);
    return 0;
}

int kl_scotland2_start(void) {
    if (!kl_env_on("KL_SCOTLAND2", 0)) return 0;
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "beatsaber140")) return stop("this loader milestone supports beatsaber140 only");
    if (started) return stop("already started in this process");
    const char *frameworks = getenv("KL_DYLIB_DIR");
    if (!frameworks || !*frameworks) return stop("KL_DYLIB_DIR is required; use embedded signed frameworks");
    char path[2048], metadata[2048];
    snprintf(path, sizeof path, "%s/Scotland2Core.framework/Scotland2Core", frameworks);
    void *core = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!core) return stop(dlerror());
    int (*configure)(const sc2_platform_api *, const char *, const char *, void *);
    int (*open_libraries)(void), (*open_early)(void);
    int (*set_paths)(const char *, const char *, const char *, const char *);
#define BIND(var, name) do { *(void **)&var = dlsym(core, name); if (!var) return stop("native core missing " name); } while (0)
    BIND(configure, "sc2_apple_configure");
    BIND(open_libraries, "sc2_apple_open_libraries");
    BIND(open_early, "sc2_apple_open_early");
    BIND(after_init, "sc2_apple_after_il2cpp_init");
    BIND(before_scene, "sc2_apple_before_first_scene");
    BIND(set_handles, "sc2_apple_set_game_handles");
    BIND(core_error, "sc2_apple_error");
    BIND(set_paths, "sc2_apple_set_runtime_paths");
#undef BIND
    sc2_platform_api api = {
        .open=kl_scotland2_open_module, .symbol=kl_shim_lookup("dlsym"),
        .error=kl_shim_lookup("dlerror"), .close=kl_shim_lookup("dlclose"), .owns=owns,
    };
    const char *root = getenv("KL_SCOTLAND2_ROOT");
    if (!root || !*root) {
        snprintf(metadata, sizeof metadata, "%s/../Scotland2Metadata", frameworks);
        root = metadata;
    }
    if (configure(&api, root, "com.beatgames.beatsaber", kl_jni_vm())) return stop(core_error());
    // Match Context.getFilesDir/getExternalFilesDir: kl_jni_files_dir is the
    // emulated storage root, while the application's persistent data is /files.
    // Mods that back up the sibling no_backup directory depend on this shape.
    const char *files=kl_jni_files_dir();
    char app_files[4096];
    int app_n=snprintf(app_files,sizeof app_files,"%s/files",kl_jni_files_dir());
    if(app_n<0 || (size_t)app_n>=sizeof app_files)return stop("application files path too long");
    if(mkdir(app_files,0755) && errno!=EEXIST)return stop("cannot create application files directory");
    struct stat app_stat;
    if(stat(app_files,&app_stat) || !S_ISDIR(app_stat.st_mode))return stop("application files path is not a directory");
    if (set_paths(app_files, app_files, path, "libil2cpp.so")) return stop(core_error());
    // Import resolution must see the C API before any mod constructors run.
    native_core = core;
    if (kl_env_on("KL_SCOTLAND2_NATIVE_GATES",0)) {
        if (kl_aot_register_native_gate(native_abort_gate,&abort_dispatch,abort)) return stop(kl_aot_error());
        native_abort_registered=1;
    }
    if (kl_env_on("KL_SCOTLAND2_ABI", 0)) {
        char adapter[4096];
        int size = snprintf(adapter, sizeof adapter, "%s/shims/libsl2_abi.so", root);
        if (size < 0 || (size_t)size >= sizeof adapter) return stop("Android path adapter location too long");
        android_paths = api.open(adapter, 2);
        if (!android_paths) return stop(api.error());
        if (android_paths != kl_find_image("libsl2_abi.so") || kl_get_stats(android_paths)->imports_missing)
            return stop("Android path adapter not registered or has unresolved imports");
        kl_image *cpp = kl_find_image("libc++_shared.so");
        if (!cpp || kl_get_stats(cpp)->imports_missing) return stop("Android C++ runtime missing or has unresolved imports");
        if (diagnostic()) fprintf(stderr, "[scotland2] ANDROID_PATHS_READY real guest C++ objects; full C++ registry/hook APIs remain unsupported\n");
    }
    if (kl_env_on("KL_SCOTLAND2_FLAMINGO",0)) {
        if (!android_paths) return stop("Flamingo requires the Android ABI support module");
        char library[4096];
        const char *flamingo_name=getenv("KL_SCOTLAND2_FLAMINGO_LIBRARY");
        if (!flamingo_name || !*flamingo_name) flamingo_name="libsc2_flamingo.so";
        if (strcmp(flamingo_name,"libsc2_flamingo.so") && strcmp(flamingo_name,"libflamingo.so"))
            return stop("unsupported Flamingo library name");
        int size=snprintf(library,sizeof library,"%s/shims/%s",root,flamingo_name);
        if (size<0 || (size_t)size>=sizeof library) return stop("Flamingo support path too long");
        android_flamingo=api.open(library,2);
        if (!android_flamingo || kl_get_stats(android_flamingo)->imports_missing) return stop("Flamingo source port missing or has unresolved imports");
        if (kl_env_on("KL_SCOTLAND2_FLAMINGO_PROBE",0)) {
            size=snprintf(library,sizeof library,diagnostic()?"%s/shims/libsc2_flamingo_probe.so":"%s/shims/libsc2_scene.so",root);
            if (size<0 || (size_t)size>=sizeof library) return stop("Flamingo probe path too long");
            android_flamingo_probe=api.open(library,2);
            if (!android_flamingo_probe || kl_get_stats(android_flamingo_probe)->imports_missing)
                return stop("Flamingo test consumer missing or has unresolved imports");
        }
        if (diagnostic()) fprintf(stderr,"[scotland2] FLAMINGO_READY original installer with signed-entry platform backend; full mod stack remains unverified\n");
    }
    if (open_libraries()) return stop(core_error());
    if (kl_env_on("KL_SCOTLAND2_FILESYSTEM",0)) {
        kl_image *bs=kl_find_image("libbeatsaber-hook.so");
        void *target=bs?kl_sym(bs,"_Z6mkpathNSt6__ndk117basic_string_viewIcNS_11char_traitsIcEEEE"):NULL;
        void *replacement=android_paths?kl_sym(android_paths,"sc2_bshook_mkpath"):NULL;
        void *original=NULL;
        if(!target || !replacement || !kl_aot_original(target) || kl_aot_exchange(target,replacement,&original))
            return stop("beatsaber-hook filesystem helper missing or not prepared");
        if (diagnostic()) fprintf(stderr,"[scotland2] FILESYSTEM_READY upstream mkpath uses direct guest directory creation through signed prepared entry\n");
    }
    if (kl_env_on("KL_SCOTLAND2_DOWNLOAD_BOUNDARIES",0)) {
        kl_image *library=kl_find_image("libbeatsaverplusplus.so");
        void *target=library?kl_sym(library,"_ZN9BeatSaver5Utils10ExtractAllENSt6__ndk14spanIKhLm18446744073709551615EEERKNS1_4__fs10filesystem4pathE"):NULL;
        void *replacement=android_paths?kl_sym(android_paths,"sc2_beatsaver_extract"):NULL;
        if(!target || !replacement || !kl_aot_original(target) || kl_aot_exchange(target,replacement,NULL))
            return stop("BeatSaverPlusPlus extraction boundary missing or not prepared");
    }
    if(diagnostic() && kl_env_on("KL_SCOTLAND2_REMAINING_CORE",0)) {
        int n=snprintf(remaining_profile_root,sizeof remaining_profile_root,"%s/profiles",root);
        if(n<0 || (size_t)n>=sizeof remaining_profile_root)return stop("remaining core profiles path too long");
    }
    if (open_early()) return stop(core_error());
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_CUSTOMTYPES_PROBE",0)) {
        kl_image *ct=kl_find_image("libcustom-types.so");
        if (!ct || kl_get_stats(ct)->imports_missing) return stop("custom-types source port missing or has unresolved imports");
        int size=snprintf(customtypes_probe_path,sizeof customtypes_probe_path,"%s/shims/libsc2_customtypes_probe.so",root);
        if (size<0 || (size_t)size>=sizeof customtypes_probe_path) return stop("custom-types probe path too long");
        fprintf(stderr,"[scotland2] CUSTOMTYPES_READY actual source port, %u imports resolved; registration awaits the original scene phase\n",kl_get_stats(ct)->imports_bound);
    }
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_LAPIZ_PROBE",0)) {
        kl_image *lapiz=kl_find_image("liblapiz.so");
        if(!lapiz || kl_get_stats(lapiz)->imports_missing)return stop("Lapiz source port missing or has unresolved imports");
        int n=snprintf(lapiz_profile_path,sizeof lapiz_profile_path,"%s/profiles/lapiz-hooks.txt",root);
        if(n<0 || (size_t)n>=sizeof lapiz_profile_path)return stop("Lapiz profile path too long");
        n=snprintf(customtypes_probe_path,sizeof customtypes_probe_path,"%s/shims/libsc2_customtypes_probe.so",root);
        if(n<0 || (size_t)n>=sizeof customtypes_probe_path)return stop("Lapiz type probe path too long");
        fprintf(stderr,"[scotland2] LAPIZ_READY actual source port, %u imports resolved; callback awaits IL2CPP initialization\n",kl_get_stats(lapiz)->imports_bound);
    }
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_BSML_PROBE",0)) {
        kl_image *bsml=kl_find_image("libbsml.so");
        kl_image *lapiz=kl_find_image("liblapiz.so");
        if(!bsml || !lapiz || kl_get_stats(bsml)->imports_missing || kl_get_stats(lapiz)->imports_missing)
            return stop("BSML/Lapiz missing or unresolved imports");
        const char *suffixes[]={"profiles/lapiz-hooks.txt","profiles/bsml-hooks.txt","shims/libsc2_customtypes_probe.so","shims/libsc2_bsml_probe.so"};
        char *paths[]={lapiz_profile_path,bsml_profile_path,customtypes_probe_path,bsml_probe_path};
        for (int i=0;i<4;++i) {
            int n=snprintf(paths[i],4096,"%s/%s",root,suffixes[i]);
            if(n<0 || n>=4096)return stop("BSML probe path too long");
        }
        fprintf(stderr,"[scotland2] BSML_READY actual source port, %u imports resolved; original callbacks await game initialization\n",kl_get_stats(bsml)->imports_bound);
    }
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_SONGCORE_PROBE",0)) {
        if(!*bsml_profile_path)return stop("SongCore test requires the BSML composition");
        int n=snprintf(songcore_profile_path,sizeof songcore_profile_path,"%s/profiles/songcore-hooks.txt",root);
        if(n<0 || (size_t)n>=sizeof songcore_profile_path)return stop("SongCore profile path too long");
        n=snprintf(songcore_probe_path,sizeof songcore_probe_path,"%s/shims/libsc2_songcore_probe.so",root);
        if(n<0 || (size_t)n>=sizeof songcore_probe_path)return stop("SongCore consumer path too long");
        fprintf(stderr,"[scotland2] SONGCORE_PENDING actual source module reserved for original late loading phase\n");
    }
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_METACORE_PROBE",0)) {
        kl_image *meta=kl_find_image("libmetacore.so");
        if(!meta || kl_get_stats(meta)->imports_missing || !native_abort_registered)
            return stop("MetaCore source module or signed native dispatcher missing");
        int n=snprintf(metacore_profile_path,sizeof metacore_profile_path,"%s/profiles/metacore-hooks.txt",root);
        if(n<0 || (size_t)n>=sizeof metacore_profile_path)return stop("MetaCore profile path too long");
        n=snprintf(metacore_probe_path,sizeof metacore_probe_path,"%s/shims/libsc2_metacore_probe.so",root);
        if(n<0 || (size_t)n>=sizeof metacore_probe_path)return stop("MetaCore consumer path too long");
        fprintf(stderr,"[scotland2] METACORE_READY %u imports resolved; original late callback pending\n",kl_get_stats(meta)->imports_bound);
    }
    if (diagnostic() && kl_env_on("KL_SCOTLAND2_SONGDOWNLOADER_PROBE",0)) {
        int n=snprintf(playlistcore_profile_path,sizeof playlistcore_profile_path,"%s/profiles/playlistcore-hooks.txt",root);
        if(n<0 || (size_t)n>=sizeof playlistcore_profile_path)return stop("PlaylistCore profile path too long");
        songdownloader_probe=1;
    }
    kl_image *image = kl_find_image("libtinyxml2.so");
    if (!image || kl_get_stats(image)->imports_missing) return stop("tinyxml2 missing or has unresolved imports");
    bool (*to_int)(const char *, int *) = kl_sym(image, "_ZN8tinyxml27XMLUtil5ToIntEPKcPi");
    int number = 0;
    if (!to_int || !to_int("1234", &number) || number != 1234) return stop("tinyxml2 execution check failed");
    if (diagnostic() && exercise_paper(files)) return -1;
    started = 1;
    if (diagnostic()) fprintf(stderr, "[scotland2] LIBRARIES_OK actual tinyxml2 executed; waiting for il2cpp_init\n");
    return 0;
}

static int probe_game_hooks(void *expected) {
    if (!diagnostic() || !kl_env_on("KL_SCOTLAND2_AOT_PROBE",0)) return 0;
    if (!android_paths) return stop("And64 guest source adapter is required for the game hook probe");
    void (*install)(void *,void *,void **)=kl_sym(android_paths,"A64HookFunction");
    void *original=kl_aot_original(domain_get);
    if (!install || !original) return stop("game hook target is not prepared or And64 adapter missing");
    unsigned char entry[4],gate[48];
    memcpy(entry,domain_get,4); memcpy(gate,(char *)original-16,48);
    install(domain_get,domain_first,(void **)&domain_original);
    if (!domain_original || domain_get()!=expected || !__atomic_load_n(&domain_first_calls,__ATOMIC_RELAXED))
        return stop("first actual IL2CPP hook/original call failed");
    install(domain_get,domain_second,(void **)&domain_previous);
    if (domain_previous!=(void *)domain_first || domain_get()!=expected || !__atomic_load_n(&domain_second_calls,__ATOMIC_RELAXED))
        return stop("actual IL2CPP hook chain failed");
    if (kl_aot_restore(domain_get) || domain_get()!=expected || memcmp(entry,domain_get,4) || memcmp(gate,(char *)original-16,48))
        return stop("actual IL2CPP hook restore or code integrity failed");
    fprintf(stderr,"[scotland2] AOT_GAME_HOOK_OK source-ported And64 hooked actual il2cpp_domain_get twice, called originals and restored; executable bytes unchanged\n");
    if (kl_env_on("KL_SCOTLAND2_FLAMINGO_PROBE",0)) {
        int (*probe)(void *,void *)=android_flamingo_probe?kl_sym(android_flamingo_probe,"sc2_flamingo_game_probe"):NULL;
        if (!probe || probe(domain_get,expected) || memcmp(entry,domain_get,4) || memcmp(gate,(char *)original-16,48))
            return stop("Flamingo actual IL2CPP chain/uninstall or code integrity failed");
        fprintf(stderr,"[scotland2] FLAMINGO_GAME_HOOK_OK original Flamingo installer hooked actual il2cpp_domain_get, preserved originals and uninstalled; executable bytes unchanged\n");
    }
    return 0;
}
static int after_original_scene_call(void) {
    if (before_scene()) return stop(core_error());
#if SC2_DIAGNOSTICS
    if (!diagnostic()) return 0;
    fprintf(stderr,"[scotland2] LATE_PHASE_OK original DestroyObjectHighLevel call completed, Flamingo hook uninstalled, Scotland2 late phase dispatched\n");
    if (*metacore_profile_path) {
        if(verify_mod_hooks(metacore_profile_path,"libmetacore.so",36))return -1;
        const char *owner=kl_addr_image(kl_aot_current(native_abort_gate),NULL);
        if(!owner || strcmp(owner,"libmetacore.so"))return stop("MetaCore abort hook missing");
        kl_image *unity=kl_find_image("libunity.so");
        void *deletion=unity?(char*)kl_base(unity)+0x511008:NULL;
        owner=deletion?kl_addr_image(kl_aot_current(deletion),NULL):NULL;
        if(!owner || strcmp(owner,"libmetacore.so"))return stop("MetaCore Unity deletion hook missing");
        kl_image *probe=kl_scotland2_open_module(metacore_probe_path,2);
        int (*run)(void)=probe?kl_sym(probe,"sc2_test_metacore_late"):NULL;
        if(!probe || kl_get_stats(probe)->imports_missing || !run)return stop("MetaCore lifecycle consumer missing");
        int result=run();
        if(result){fprintf(stderr,"[scotland2] MetaCore lifecycle result %d\n",result);return stop("MetaCore real Unity lifecycle check failed");}
        fprintf(stderr,"[scotland2] METACORE_LATE_OK original callback, 36 game hooks, signed native abort hook and real Unity destruction callback; scheduler observation queued\n");
    }
    if (*bsml_probe_path) {
        kl_image *probe=kl_scotland2_open_module(bsml_probe_path,2);
        int (*run)(void)=probe?kl_sym(probe,"sc2_test_bsml_late"):NULL;
        if(!probe || kl_get_stats(probe)->imports_missing || !run)return stop("BSML lifecycle consumer unavailable");
        int result=run();
        if(result){fprintf(stderr,"[scotland2] BSML late consumer result %d\n",result);return stop("BSML Unity lifecycle test failed");}
        fprintf(stderr,"[scotland2] BSML_LATE_OK original late callback created both Unity GameObjects/components; coroutine Awake singleton matches\n");
    }
    if (*songcore_profile_path) {
        kl_image *songcore=kl_find_image("libsongcore.so");
        if(!songcore || kl_get_stats(songcore)->imports_missing)return stop("SongCore missing after original late callback or unresolved imports");
        if(verify_mod_hooks(songcore_profile_path,"libsongcore.so",33))return -1;
        kl_image *types=kl_scotland2_open_module(customtypes_probe_path,2);
        int (*check_types)(void)=types?kl_sym(types,"sc2_test_songcore_types"):NULL;
        if(!types || kl_get_stats(types)->imports_missing || !check_types)return stop("SongCore type consumer unavailable");
        int result=check_types();
        if(result){fprintf(stderr,"[scotland2] SongCore type result %d\n",result);return stop("SongCore registration test failed");}
        kl_image *consumer=kl_scotland2_open_module(songcore_probe_path,2);
        int (*observe)(void)=consumer?kl_sym(consumer,"sc2_test_songcore_lifecycle"):NULL;
        if(!consumer || kl_get_stats(consumer)->imports_missing || !observe || observe())return stop("SongCore lifecycle consumer failed");
        fprintf(stderr,"[scotland2] SONGCORE_LATE_OK original Scotland2 late callback, %u imports resolved, 33 installed hooks and 23 registered types; service/scan observation scheduled\n",kl_get_stats(songcore)->imports_bound);
    }
    if (*customtypes_probe_path && !*lapiz_profile_path) {
        void *(*open)(const char *,int)=kl_shim_lookup("dlopen");
        kl_image *probe=open(customtypes_probe_path,2);
        if (!probe || kl_get_stats(probe)->imports_missing) return stop("custom-types registration consumer failed to load");
        int (*run)(void)=kl_sym(probe,"sc2_test_customtypes_register");
        if (!run || run()) return stop("actual custom-types registration/name/type/allocation test failed");
        fprintf(stderr,"[scotland2] CUSTOMTYPES_REGISTRATION_OK two original built-in classes registered once, found through actual game name/type APIs and instantiated; full mod stack remains unverified\n");
    }
    if (songdownloader_probe) {
        if(verify_mod_hooks(playlistcore_profile_path,"libplaylistcore.so",8))return -1;
        kl_image *types=kl_scotland2_open_module(customtypes_probe_path,2);
        int (*playlist_types)(void)=types?kl_sym(types,"sc2_test_playlistcore_types"):NULL;
        int (*browser_types)(void)=types?kl_sym(types,"sc2_test_songdownloader_types"):NULL;
        if(!playlist_types || !browser_types || playlist_types() || browser_types())return stop("PlaylistCore/SongDownloader type check failed");
        kl_image *browser=kl_find_image("libsongdownloader.so");
        int (*run)(void)=browser?kl_sym(browser,"sc2_test_songdownloader_late"):NULL;
        if(!browser || kl_get_stats(browser)->imports_missing || !run || run())return stop("SongDownloader original menu registration missing");
        fprintf(stderr,"[scotland2] SONGDOWNLOADER_LATE_OK actual original callbacks, eight PlaylistCore hooks and five/four custom types; in-game browsing/download/playback still require interaction\n");
    }
    if(*remaining_profile_root){
        const char *mods[]={"datakeeper","unicode"};
        const char *libs[]={"libdatakeeper.so","libunicode.so"};
        const unsigned counts[]={4,4};
        for(unsigned i=0;i<sizeof mods/sizeof mods[0];++i){
            char profile[4096];int n=snprintf(profile,sizeof profile,"%s/%s-hooks.txt",remaining_profile_root,mods[i]);
            kl_image *mod=kl_find_image(libs[i]);
            if(n<0 || (size_t)n>=sizeof profile || !mod || kl_get_stats(mod)->imports_missing)return stop("remaining core module missing or unresolved imports");
            if(verify_mod_hooks(profile,libs[i],counts[i]))return -1;
            fprintf(stderr,"[scotland2] COREMOD_READY %s imports=%u hooks=%u original callbacks completed\n",mods[i],kl_get_stats(mod)->imports_bound,counts[i]);
        }
        kl_image *unicode=kl_find_image("libunicode.so");
        int (*observe)(void)=kl_sym(unicode,"sc2_test_unicode_lifecycle");
        if(!observe||observe())return stop("Unicode platform glyph bridge failed");
        fprintf(stderr,"[scotland2] REMAINING_CORE_LATE_OK both selected original modules and eight hooks; Fake ID excluded; Unicode Unity glyph observation scheduled\n");
    }
#endif
    return 0;
}
static int unity_address(const void *address,size_t size) {
    kl_image *image=kl_find_image("libunity.so");
    if (!image) return 0;
    uintptr_t base=(uintptr_t)kl_base(image),p=(uintptr_t)address;
    return size<=kl_span(image) && p>=base && p-base<=kl_span(image)-size;
}
static uint32_t *direct_target(uint32_t *pc,uint32_t word) {
    int32_t scaled=(int32_t)((word&0x3ffffffu)<<6);
    return (uint32_t *)((uintptr_t)pc+((intptr_t)scaled>>4));
}
static int install_original_scene_hook(void) {
    if (!kl_env_on("KL_SCOTLAND2_SCENE_HOOK",0)) return 0;
    if (!android_flamingo_probe) return stop("original scene hook requires the Flamingo guest bridge");
    kl_image *il2cpp=kl_find_image("libil2cpp.so");
    void *(*resolve)(const char *)=kl_sym(il2cpp,"il2cpp_resolve_icall");
    uint32_t *destroy=resolve?resolve("UnityEngine.Object::DestroyImmediate"):NULL;
    if (!destroy && resolve) destroy=resolve("UnityEngine.Object::DestroyImmediate_Injected");
    if (!destroy || !unity_address(destroy,100*4)) return stop("Unity DestroyImmediate icall unavailable or outside libunity");
    // Same lookup as upstream main.cpp: first RET, preceding last BL, first B.
    uint32_t *ret=NULL,*scripting=NULL,*target=NULL;
    for (int i=0;i<100;++i) if (destroy[i]==0xd65f03c0u) { ret=destroy+i; break; }
    if (!ret) return stop("bounded DestroyImmediate RET not found");
    for (uint32_t *pc=ret;pc>destroy;--pc) if ((*pc>>26)==0b100101) { scripting=direct_target(pc,*pc); break; }
    if (!scripting || !unity_address(scripting,4096)) return stop("bounded scripting destroy target not found");
    for (int i=0;i<1024;++i) if ((scripting[i]>>26)==0b000101) { target=direct_target(scripting+i,scripting[i]); break; }
    if (!target || !unity_address(target,4) || !kl_aot_original(target))
        return stop("runtime DestroyObjectHighLevel does not match a prepared signed target");
    int (*install)(void *,int (*)())=kl_sym(android_flamingo_probe,"sc2_install_original_scene_hook");
    if (!install || install(target,after_original_scene_call)) return stop("original Flamingo scene-hook installation failed");
    fprintf(stderr,"[scotland2] SCENE_HOOK_ARMED actual icall resolved; prepared libunity target +0x%zx; original Scotland2 timing\n",(size_t)((uintptr_t)target-(uintptr_t)kl_base(kl_find_image("libunity.so"))));
    return 0;
}
#if SC2_DIAGNOSTICS
static int verify_mod_hooks(const char *path,const char *module,unsigned expected) {
    return kl_scotland2_verify_mod_hooks(path,module,expected);
}
int kl_scotland2_verify_mod_hooks(const char *path,const char *module,unsigned expected) {
    FILE *profile=fopen(path,"r");
    if(!profile)return stop("mod hook profile missing");
    kl_image *probe=*customtypes_probe_path?kl_scotland2_open_module(customtypes_probe_path,2):NULL;
    const void *(*previous)(const void*,const void*)=probe?kl_sym(probe,"sc2_test_hook_previous"):NULL;
    unsigned long long offset;unsigned word,count=0;char name[128];
    kl_image *game=kl_find_image("libil2cpp.so");
    while(fscanf(profile,"%llx %x %127s",&offset,&word,name)==3) {
        if(offset>kl_span(game)-4){fclose(profile);return stop("mod target outside game");}
        void *target=(char*)kl_base(game)+offset;uint32_t original_word=0;
        void *entry=target,*call=kl_aot_current(entry);int found=0;
        for(unsigned hop=0;call && hop<128;++hop) {
            const char *owner=kl_addr_image(call,NULL);
            if(owner&&!strcmp(owner,module)){found=1;break;}
            void *prior=previous?(void*)previous(entry,call):NULL;
            if(prior && prior!=call) {call=prior;continue;}
            entry=call;call=kl_aot_current(entry);
        }
        if(kl_aot_entry_word(target,&original_word) || original_word!=word || !found) {
            fprintf(stderr,"[scotland2] Hook verification failed: %s %s +0x%llx\n",module,name,offset);
            fclose(profile);return stop("mod callback did not install expected target");
        }
        ++count;
    }
    int valid=feof(profile)&&count==expected;fclose(profile);
    return valid?0:stop("mod hook profile count/format mismatch");
}
#endif
static void wrapped_init(const char *domain) {
    real_init(domain);
    // Read the real domain instead of interpreting an unspecified return register.
    void *actual_domain=domain_get();
    if (actual_domain && !init_finished) {
        init_finished = 1;
        if (probe_game_hooks(actual_domain)) abort();
        if (after_init() != 0) { stop(core_error()); abort(); }
#if SC2_DIAGNOSTICS
        if (*lapiz_profile_path && verify_mod_hooks(lapiz_profile_path,"liblapiz.so",31)) abort();
        if (*bsml_profile_path && verify_mod_hooks(bsml_profile_path,"libbsml.so",13)) abort();
        if (*lapiz_profile_path) {
            kl_image *probe=kl_scotland2_open_module(customtypes_probe_path,2);
            const char *symbol=*bsml_profile_path?"sc2_test_bsml_types":"sc2_test_lapiz_types";
            int (*run)(void)=probe?kl_sym(probe,symbol):NULL;
            if(!probe || kl_get_stats(probe)->imports_missing || !run){stop("mod type consumer unavailable");abort();}
            int result=run();
            if(result){fprintf(stderr,"[scotland2] Type consumer result %d\n",result);stop("mod type test failed");abort();}
            if (*bsml_profile_path)
                fprintf(stderr,"[scotland2] BSML_EARLY_OK actual early callbacks, 31 Lapiz and 13 BSML installed hooks, 61 BSML types resolved; repeat registration has no duplicates\n");
            else
                fprintf(stderr,"[scotland2] LAPIZ_EARLY_OK actual load callback, 31 installed game targets, 20 mod types resolved, scheduler allocated and Tick called; repeat registration has no duplicates\n");
        }
#endif
        if (install_original_scene_hook()) abort();
        if (diagnostic()) fprintf(stderr, "[scotland2] IL2CPP_PHASE_OK early phase dispatched; scene hook %s\n",kl_env_on("KL_SCOTLAND2_SCENE_HOOK",0)?"armed":"disabled");
    }
}
void *kl_scotland2_interpose(void *handle, const char *name, void *real) {
    // Once the one-time phase transition is complete, SDK code inspectors must
    // receive the real guest entry, not this native bootstrap wrapper.
    if (!started || init_finished || !real || strcmp(name, "il2cpp_init") || handle != kl_find_image("libil2cpp.so")) return NULL;
    if (!real_init && set_handles(handle, kl_find_image("libunity.so")) != 0) {
        stop(core_error()); abort();
    }
    domain_get = kl_sym((kl_image *)handle, "il2cpp_domain_get");
    if (!domain_get) { stop("missing il2cpp_domain_get"); abort(); }
    real_init = (void (*)(const char *))real;
    return (void *)wrapped_init;
}

// Registered before guest loading; the generic host holds no Scotland2 policy.
void kl_guest_extension_builtin(void) {
    static const kl_guest_extension api = {
        .version=1, .size=sizeof(kl_guest_extension),
        .start=kl_scotland2_start, .error=kl_scotland2_error,
        .symbol=kl_scotland2_sym, .claims=kl_scotland2_claims,
        .open=kl_scotland2_dlopen, .is_handle=kl_scotland2_is_handle,
        .interpose=kl_scotland2_interpose, .strict_load=kl_scotland2_strict_load,
    };
    if (kl_extension_register(&api)) abort();
}
