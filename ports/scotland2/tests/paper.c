// Exercise the released Rust Paper logger through the real Scotland2 core.
#include "klepton.h"
#include "kl_driver.h"
#include "kl_jni.h"
#include "kl_scotland2.h"
#include "kl_aot_hooks.h"
#include "../../../port-deps/sources/paperlog/shared/bindings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

static uint64_t (*and64_original)(uint64_t),(*and64_previous)(uint64_t);
static uint64_t and64_first(uint64_t value) { return and64_original(value)+1000; }
static uint64_t and64_second(uint64_t value) { return and64_previous(value)+10000; }
static uint64_t (*scene_count)(void);
static void *scene_target;
static int scene_callbacks;
static int observe_scene(void) {
    assert(scene_count && scene_count()==1);
    assert(kl_aot_current(scene_target)==kl_aot_original(scene_target));
    ++scene_callbacks; return 0;
}

static int contains(const char *path, const char *needle) {
    char text[65536];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t size = fread(text, 1, sizeof text - 1, file);
    fclose(file); text[size] = 0;
    return strstr(text, needle) != NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    kl_thread_init();
    kl_target target = {.name="beatsaber140", .kind=KL_GUEST_UNITY};
    kl_driver_init(&target, NULL, 0);
    assert(!kl_scotland2_claims("libsl2.so"));
    assert(!kl_scotland2_sym("modloader_get_application_id"));
    kl_jni_set_files_dir(argv[1]);
    kl_guest_extstorage_map(argv[1]);
    assert(kl_scotland2_start() == 0);

    void *(*open)(const char *, int) = kl_shim_lookup("dlopen");
    void *(*symbol)(void *, const char *) = kl_shim_lookup("dlsym");
    const char *(*error)(void) = kl_shim_lookup("dlerror");
    void *loader = open("libsl2.so", 2);
    assert(loader && kl_scotland2_is_handle(loader));
    const char *(*app_id)(void) = symbol(loader, "modloader_get_application_id");
    const char *(*files)(void) = symbol(loader, "modloader_get_files_dir");
    assert(app_id && !strcmp(app_id(), "com.beatgames.beatsaber"));
    char expected_files[4096];snprintf(expected_files,sizeof expected_files,"%s/files",argv[1]);
    assert(files && !strcmp(files(), expected_files));
    assert(symbol(NULL, "modloader_get_application_id") == (void *)app_id);
    if (!kl_find_image("libsl2_abi.so"))
        assert(!symbol(loader,"_ZN9modloader13get_files_dirEv"));
    assert(!symbol(loader, "_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"));
    assert(strstr(error(), "unsupported Scotland2 export"));
    kl_image *paper = kl_find_image("libpaper2_scotland2.so");
    assert(paper);
    const kl_stats *stats = kl_get_stats(paper);
    printf("Paper import inventory: %u bound, %u missing\n", stats->imports_bound, stats->imports_missing);
    assert(!stats->imports_missing);
    bool (*inited)(void) = kl_sym(paper, "paper2_get_inited");
    const char *(*directory)(void) = kl_sym(paper, "paper2_get_log_directory");
    void (*register_context)(const char *) = kl_sym(paper, "paper2_register_context_id");
    bool (*queue)(paper2_LogLevel,const char *,const char *,const char *,int,int,const char *) = kl_sym(paper, "paper2_queue_log_ffi");
    bool (*flush)(unsigned) = kl_sym(paper, "paper2_wait_flush_timeout");
    assert(inited && directory && register_context && queue && flush);
    assert(inited()); // The actual ELF constructor must initialize the logger.
    assert(!strcmp(directory(), "/sdcard/ModData/com.beatgames.beatsaber/logs2"));
    register_context("Scotland2PortTest");
    assert(queue(Info, "Scotland2PortTest", "REAL_PAPER_SCOTLAND2_MARKER_20261023", __FILE__, __LINE__, 1, "main"));
    char main_log[4096], context_log[4096];
    snprintf(main_log, sizeof main_log, "%s/ModData/com.beatgames.beatsaber/logs2/Paperlog.log", argv[1]);
    snprintf(context_log, sizeof context_log, "%s/ModData/com.beatgames.beatsaber/logs2/Scotland2PortTest.log", argv[1]);
    // Paper's timeout API waits on the worker's queue semaphore, so it can steal
    // the notification. Observe actual writes before exercising that API.
    int written = 0;
    for (int i = 0; i < 100; ++i) {
        written = contains(main_log, "REAL_PAPER_SCOTLAND2_MARKER_20261023") &&
                  contains(context_log, "REAL_PAPER_SCOTLAND2_MARKER_20261023");
        if (written) break;
        usleep(20000);
    }
    assert(written);
    assert(flush(1));
    const char *consumer_path = getenv("KL_SCOTLAND2_ABI_CONSUMER");
    if (consumer_path) {
        kl_image *adapter = kl_find_image("libsl2_abi.so");
        assert(adapter && !kl_get_stats(adapter)->imports_missing);
        const char *getters[] = {
            "_ZN9modloader13get_files_dirEv", "_ZN9modloader16get_external_dirEv",
            "_ZN9modloader18get_application_idEv", "_ZN9modloader18get_libil2cpp_pathEv",
            "_ZN9modloader18get_modloader_pathEv", "_ZN9modloader25get_modloader_source_pathEv",
            "_ZN9modloader28get_modloader_root_load_pathEv",
        };
        for (size_t i = 0; i < sizeof getters / sizeof *getters; ++i) {
            void *getter = symbol(loader, getters[i]);
            assert(getter && getter == kl_sym(adapter, getters[i]));
        }
        kl_image *consumer = open(consumer_path, 2);
        assert(consumer && !kl_get_stats(consumer)->imports_missing);
        int (*check)(void) = symbol(consumer, "sc2_test_android_paths");
        assert(check && check() == 0);
        puts("PASS: seven Android C++ getters consumed across separate signed guest libraries, copied and mutated without changing originals.");
    }
    const char *bshook_test = getenv("KL_SCOTLAND2_BSHOOK_TEST");
    if (bshook_test && !strcmp(bshook_test,"1")) {
        kl_image *bshook = kl_find_image("libbeatsaber-hook.so");
        assert(bshook);
        const kl_stats *stats = kl_get_stats(bshook);
        printf("beatsaber-hook import inventory: %u bound, %u missing\n", stats->imports_bound, stats->imports_missing);
        if (stats->imports_missing) {
            unsigned count = 0;
            const char *const *missing = kl_missing_imports(bshook, &count);
            for (unsigned i = 0; i < count; ++i) fprintf(stderr,"Missing beatsaber-hook import: %s\n",missing[i]);
        }
        assert(!stats->imports_missing);
        kl_image *cpp = kl_find_image("libc++_shared.so");
        assert(cpp && !kl_get_stats(cpp)->imports_missing);
        int (*version)(int *, int *) = symbol(loader,"cs_version");
        int major = 0, minor = 0;
        assert(version && version(&major,&minor) && major == 5 && minor == 0);
        const unsigned *(*read_branch)(const unsigned *) = kl_sym(bshook,"_ZN2cs5readbEPKj");
        assert(read_branch);
        const unsigned code[] = {0x14000002,0xd503201f,0xd65f03c0};
        assert(read_branch(code) == code + 2);
        puts("PASS: actual beatsaber-hook constructors and Capstone branch decoding; hooks were NOT installed or verified.");
    }
    const char *aot_test=getenv("KL_SCOTLAND2_AOT_TEST");
    if (aot_test && !strcmp(aot_test,"1")) {
        kl_image *adapter=kl_find_image("libsl2_abi.so");
        void (*install)(void *,void *,void **)=kl_sym(adapter,"A64HookFunction");
        void *(*install_buffer)(void *,void *,void *,uintptr_t)=kl_sym(adapter,"A64HookFunctionV");
        assert(install && install_buffer);
        kl_image *fixture=open(getenv("KL_SCOTLAND2_AOT_FIXTURE"),2);
        assert(fixture && !kl_get_stats(fixture)->imports_missing);
        kl_image *bs=kl_find_image("libbeatsaber-hook.so");assert(bs);
        const unsigned *(*read_branch)(const unsigned *)=kl_sym(bs,"_ZN2cs5readbEPKj");
        const unsigned *tail=kl_sym(fixture,"sc2_tail");
        assert(read_branch && tail && read_branch(tail)==tail+1);
        puts("PASS: actual beatsaber-hook Capstone sees the original prepared branch at its real PC.");
        uint64_t (*target)(uint64_t)=kl_sym(fixture,"sc2_cbz");
        assert(target && target(0)==42);
        install(target,and64_first,(void **)&and64_original);
        assert(and64_original && target(0)==1042 && target(9)==1010);
        install(target,and64_second,(void **)&and64_previous);
        assert(and64_previous==(void *)and64_first && target(0)==11042);
        assert(!kl_aot_restore(target) && target(0)==42);
        unsigned char buffer[128];
        assert(!install_buffer(target,and64_first,buffer,sizeof buffer) && target(0)==42);
        void *old=(void *)1;
        install(kl_sym(fixture,"sc2_unprepared"),and64_first,&old);
        assert(!old);
        puts("PASS: source-ported And64 API installs/chains/restores signed hooks; unsupported caller buffers and unprepared targets fail without installing.");
        if (getenv("KL_SCOTLAND2_SDK_HOOK_PROBE") && !strcmp(getenv("KL_SCOTLAND2_SDK_HOOK_PROBE"),"1")) {
            char path[4096];snprintf(path,sizeof path,"%s/test-only/libsc2_sdk_hooks.so",getenv("KL_SCOTLAND2_ROOT"));
            kl_image *sdk=open(path,2);assert(sdk && !kl_get_stats(sdk)->imports_missing);
            int (*test_sdk)(void *)=kl_sym(sdk,"sc2_test_sdk_original_hooks");assert(test_sdk);
            int result=test_sdk(target);fprintf(stderr,"SDK original-hook test result: %d\n",result);assert(!result);
            puts("PASS: actual beatsaber-hook SDK InstallHook/InstallOrigHook twice, shared HookTracker and signed original-entry dispatch.");
        }
        const char *flamingo_test=getenv("KL_SCOTLAND2_FLAMINGO_PROBE");
        if (flamingo_test && !strcmp(flamingo_test,"1")) {
            kl_image *flamingo=kl_find_image("libsc2_flamingo_probe.so");
            int (*test)(void *,void *)=flamingo?kl_sym(flamingo,"sc2_test_flamingo"):NULL;
            int result=test?test(target,kl_sym(fixture,"sc2_unprepared")):-1;
            if (result) fprintf(stderr,"Flamingo guest C++ test failed at case %d\n",result);
            assert(result==0);
            install(target,and64_first,(void **)&and64_original);
            assert(target(0)==1042 && test(target,kl_sym(fixture,"sc2_unprepared"))==0 && target(0)==1042);
            assert(!kl_aot_restore(target) && target(0)==42);
            puts("PASS: original Flamingo installer/default-final priorities, metadata rejection, original calls, reinstall and middle/head/last uninstall across separate Android DSOs.");
            scene_count=kl_sym(fixture,"sc2_scene_count");
            scene_target=kl_sym(fixture,"sc2_scene_fixture");
            int (*install_scene)(void *,int (*)())=kl_sym(flamingo,"sc2_install_original_scene_hook");
            assert(scene_count && scene_target && install_scene && scene_count()==0);
            assert(!install_scene(scene_target,observe_scene));
            void (*destroy)(void *,bool)=scene_target;
            destroy((void *)0x1234,true);
            assert(scene_count()==1 && scene_callbacks==1);
            destroy((void *)0x1234,false);
            assert(scene_count()==2 && scene_callbacks==1);
            puts("PASS: original Scotland2 scene-hook order: real fixture original, Flamingo self-uninstall, callback exactly once.");
        }
    }
    printf("PASS: real Paper 4.6.4 constructor, Scotland2 C import, background logger and both log files.\nEvidence: %s\n", main_log);
    return 0;
}
