#include "klepton.h"
#include "kl_driver.h"
#include "kl_scotland2.h"
#include "kl_aot_hooks.h"
#include "kl_jni.h"
#include "kl_obbmap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>

static void import_abort_trace(int signal_number) {
    void *frames[80];int count=backtrace(frames,80);
    for(int i=0;i<count;++i) {
        size_t offset=0;const char *image=kl_addr_image(frames[i],&offset);
        if(image) fprintf(stderr,"IMPORT_ABORT_FRAME %d %s +0x%zx\n",i,image,offset);
        else fprintf(stderr,"IMPORT_ABORT_NATIVE %d %p\n",i,frames[i]);
    }
    _Exit(128+signal_number);
}

static void verify_mod_hooks(kl_image *game,const char *profile_path,const char *owner,unsigned expected) {
    FILE *file=fopen(profile_path,"r");assert(file);
    unsigned long long offset;unsigned word,count=0;char name[128];
    while(fscanf(file,"%llx %x %127s",&offset,&word,name)==3) {
        assert(offset<=kl_span(game)-4);
        void *target=(char*)kl_base(game)+offset;uint32_t original=0;
        const char *actual=kl_addr_image(kl_aot_current(target),NULL);
        assert(!kl_aot_entry_word(target,&original) && original==word);
        if(!actual || strcmp(actual,owner))fprintf(stderr,"Uninstalled mod hook: %s +0x%llx\n",name,offset);
        assert(actual && !strcmp(actual,owner));++count;
    }
    assert(feof(file)&&count==expected);fclose(file);
}

static void check_safe_extract(int (*extract)(const char*,size_t,const char*), const char* label) {
    const char *good=getenv("SC2_TEST_ZIP_PATH"), *bad=getenv("SC2_TEST_BAD_ZIP_PATH");
    assert(good && bad);
    char root[]="/tmp/sc2-extract-XXXXXXXX"; assert(mkdtemp(root));
    char dest[256],bad_dest[256],file[300];snprintf(bad_dest,sizeof bad_dest,"%s/bad-map",root);snprintf(dest,sizeof dest,"%s/map",root);snprintf(file,sizeof file,"%s/payload.txt",dest);
    for (int trial=0;trial<3;++trial) {
        FILE *f=fopen(trial==2?bad:good,"rb");assert(f && !fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0 && size<1024*1024 && !fseek(f,0,SEEK_SET));
        char *bytes=malloc((size_t)size);assert(bytes && fread(bytes,1,(size_t)size,f)==(size_t)size);fclose(f);
        int result=extract(bytes,(size_t)size,trial==2?bad_dest:dest);free(bytes);
        assert(trial==0?result==0:result!=0);
        f=fopen(file,"rb");assert(f);char data[4096];size_t count=fread(data,1,sizeof data,f);fclose(f);
        const char expected[]="SCOTLAND2_REAL_ZIP_DEFLATE\n";assert(count==(sizeof expected-1)*100);
        for(unsigned i=0;i<100;++i)assert(!memcmp(data+i*(sizeof expected-1),expected,sizeof expected-1));
    }
    assert(!unlink(file) && !rmdir(dest) && !rmdir(root));
    fprintf(stderr,"%s_SAFE_EXTRACT_OK translated original entry extracts a real map and preserves existing data on duplicate/malicious inputs\n",label);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    kl_thread_init();
    kl_target target = {.name="beatsaber140", .kind=KL_GUEST_UNITY};
    kl_driver_init(&target, NULL, 0);
    kl_guest_extstorage_map(kl_jni_files_dir());
    if(getenv("SC2_IMPORTS_ONLY") || getenv("SC2_INIT_ONLY") || getenv("SC2_1453_PROBE")) signal(SIGABRT,import_abort_trace);
    int start=kl_scotland2_start();
    if(getenv("SC2_TEST_REJECT_IMPORT")) {
        assert(start==-1);
        assert(strstr(kl_scotland2_error(),"sc2_intentionally_missing_import"));
        assert(!kl_find_image("libsc2_bad_import.so"));
        puts("STRICT_IMPORT_REJECTION_OK unresolved mod rejected before its trapping constructor and before registration.");
        return 0;
    }
    assert(start == 0);
    if(getenv("SC2_1453_DOWNLOAD_PROBE")) {
        kl_image *probe=kl_scotland2_open_module(getenv("SC2_1453_DOWNLOAD_PROBE"),2);
        if(!probe){fprintf(stderr,"1453 download fixture: %s\n",kl_error());return 1;}
        FILE *targets=fopen(getenv("SC2_1453_PRIVATE_TARGETS"),"r");assert(targets);
        void *private_entries[3];char module[256];unsigned long long offset;
        for(unsigned i=0;i<3;++i) {
            assert(fscanf(targets,"%255s %llx",module,&offset)==2);
            kl_image *owner=kl_find_image(module);assert(owner && offset<kl_span(owner));
            private_entries[i]=(char*)kl_base(owner)+offset;
        }
        fclose(targets);
        void (*bind)(void*,void*,void*)=kl_sym(probe,"sc2_test_1453_bind_private");assert(bind);
        bind(private_entries[0],private_entries[1],private_entries[2]);
        int (*tls)(const char*,int)=kl_sym(probe,"sc2_test_1453_download_tls");assert(tls);
        assert(!tls(getenv("SC2_1453_GOOD_TLS"),1));
        assert(!tls(getenv("SC2_1453_BAD_TLS"),0));
        assert(!tls(getenv("SC2_1453_BAD_HOST"),0));
        int (*beatsaver)(const char*,size_t,const char*)=kl_sym(probe,"sc2_test_1453_beatsaver_extract");
        int (*downloader)(const char*,size_t,const char*)=kl_sym(probe,"sc2_test_1453_downloader_extract");assert(beatsaver&&downloader);
        FILE *file=fopen(getenv("SC2_1453_BAD_ZIPS"),"r");assert(file);
        char *line=NULL;size_t capacity=0;unsigned cases=0;ssize_t length;
        while((length=getline(&line,&capacity,file))>0) {
            if(line[length-1]=='\n')line[--length]=0;
            assert(length && !setenv("SC2_TEST_BAD_ZIP_PATH",line,1));
            check_safe_extract(beatsaver,"BEATSAVER_1453");
            check_safe_extract(downloader,"SONGDOWNLOADER_1453");++cases;
        }
        assert(feof(file)&&cases);fclose(file);free(line);
        printf("DOWNLOAD_BOUNDARIES_1453_OK three actual clients accepted trusted HTTPS and rejected untrusted certificates/wrong hosts; %u ZIP attack cases rejected by both original download paths.\n",cases);
        return 0;
    }
    const char *import_list=getenv("SC2_IMPORTS_ONLY");
    if (import_list && *import_list) {
        FILE *file=fopen(import_list,"r");assert(file);
        char name[256];unsigned checked=0;
        while(fscanf(file,"%255s",name)==1) {
            assert(!strchr(name,'/'));
            kl_image *module=kl_find_image(name);
            if (!module) { fprintf(stderr,"Missing import-check module: %s\n",name);return 1; }
            unsigned missing_count=0;const char *const *missing=kl_missing_imports(module,&missing_count);
            for (unsigned i=0;i<missing_count;++i) fprintf(stderr,"Unresolved %s: %s\n",name,missing[i]);
            if(missing_count || kl_get_stats(module)->imports_missing) return 1;
            ++checked;
        }
        assert(feof(file)&&checked);fclose(file);
        printf("HOST_IMPORTS_AND_CONSTRUCTORS_OK %u real Android modules strictly loaded; no game initialization or mod callbacks invoked.\n",checked);
        return 0;
    }
    if(getenv("SC2_TEST_MISSING_DEP")) {
        assert(!kl_scotland2_open_module(getenv("SC2_TEST_MISSING_DEP"),2));
        const char *(*error)(void)=kl_shim_lookup("dlerror");
        const char *message=error();assert(message && strstr(message,"missing dependency"));
        puts("STRICT_DEPENDENCY_REJECTION_OK unused missing DT_NEEDED rejected before trapping constructor");
        return 0;
    }
    if(getenv("SC2_FILESYSTEM_PROBE")) {
        if(getenv("SC2_FS_OBBDIR")) {
            kl_obbmap_init(getenv("SC2_FS_OBBDIR"),getenv("SC2_FS_UE_BASE"));
            if(getenv("SC2_FS_UE_DIRECTORY")) {
                const char *root=getenv("SC2_FS_UE_DIRECTORY");
                void *synthetic=kl_obbmap_opendir(root);assert(synthetic);
                unsigned found=0;char name[256];int is_dir,r;
                while((r=kl_obbmap_readdir(synthetic,name,sizeof name,&is_dir))>0)
                    if(!strcmp(name,"fixture.pak")&&!is_dir)++found;
                assert(r==0&&found==1);
                assert(!kl_obbmap_closedir(synthetic));
                assert(kl_obbmap_opendir(getenv("SC2_FILESYSTEM_ROOT"))==NULL);
                puts("UE4_DIRECTORY_SYNTHESIS_OK virtual pak enumerated once; unrelated directory declined");
                return 0;
            }
            if(getenv("SC2_FS_UNITY_GUARD")) {
                void *synthetic=kl_obbmap_opendir(getenv("SC2_FILESYSTEM_ROOT"));
                if(synthetic) {
                    kl_obbmap_closedir(synthetic);
                    fputs("UNITY_DIRECTORY_GUARD_FAILED empty Unreal base incorrectly claims a real Unity directory\n",stderr);return 7;
                }
            }
        }
        kl_image *probe=kl_scotland2_open_module(getenv("SC2_FILESYSTEM_PROBE"),2);
        if(!probe){fprintf(stderr,"Filesystem probe strict load: %s\n",kl_error());return 1;}
        int (*run)(const char*)=kl_sym(probe,"sc2_test_recursive_filesystem");assert(run);
        return run(getenv("SC2_FILESYSTEM_ROOT"));
    }
    if(getenv("SC2_WEAK_FIRST")) {
        kl_image *first=kl_scotland2_open_module(getenv("SC2_WEAK_FIRST"),2);
        kl_image *second=kl_scotland2_open_module(getenv("SC2_WEAK_SECOND"),2);
        assert(first&&second);
        uint64_t *(*a)(int)=kl_sym(first,"sc2_first_weak_storage");
        uint64_t *(*b)(int)=kl_sym(second,"sc2_second_weak_storage");assert(a&&b);
        if(a(0)!=b(0)) {
            fputs("SC2_WEAK_ODR_FAILED default-visible inline storage duplicated across mod DSOs\n",stderr);
            return 1;
        }
        assert(a(1)!=b(1)&&a(2)!=b(2));
        assert(*a(0)==1); // the shared inline constructor ran once across both DSOs
        *a(0)=41;++*b(0);assert(*a(0)==42);
        puts("SC2_WEAK_ODR_OK two actual Android DSOs share default-visible inline data; hidden/protected storage remains local.");
        return 0;
    }
    kl_image *image = kl_load_auto(argv[1]);
    if (!image) { fprintf(stderr, "%s\n", kl_error()); return 1; }
    assert(kl_get_stats(image)->imports_missing == 0);
    kl_register_image("libil2cpp.so", image);
    kl_run_init(image);
    if(getenv("SC2_1453_PROBE")) {
        kl_image *unity=kl_load_auto(getenv("SC2_1453_UNITY"));
        if(!unity){fprintf(stderr,"1453 Unity load: %s\n",kl_error());return 1;}
        unsigned n=0;const char *const *missing=kl_missing_imports(unity,&n);
        for(unsigned i=0;i<n;++i)fprintf(stderr,"Unity unresolved: %s\n",missing[i]);
        // Android's debugger check is not used by these initialization checks.
        assert(n==1 && !strcmp(missing[0],"ptrace"));
        kl_register_image("libunity.so",unity);
    }
    if (getenv("SC2_LAPIZ_CATALOG")) {
        // Lapiz registers custom types during the real early phase. Its Unity
        // hook target must already be mapped, as on device; do not start Unity.
        kl_image *unity=kl_load_auto(getenv("KL_SCOTLAND2_CUSTOMTYPES_UNITY"));
        assert(unity);
        unsigned n=0;const char *const *missing=kl_missing_imports(unity,&n);
        assert(n==1 && !strcmp(missing[0],"ptrace"));
        kl_register_image("libunity.so",unity);
        kl_image *lapiz=kl_find_image("liblapiz.so");assert(lapiz);
        const kl_stats *s=kl_get_stats(lapiz);
        fprintf(stderr,"Lapiz before callback: %u imports bound, %u missing\n",s->imports_bound,s->imports_missing);
        if(s->imports_missing) {
            missing=kl_missing_imports(lapiz,&n);
            for(unsigned i=0;i<n;++i)fprintf(stderr,"Missing Lapiz import: %s\n",missing[i]);
            return 1;
        }
        if(getenv("SC2_BSML_CATALOG")) {
            kl_image *bsml=kl_find_image("libbsml.so");assert(bsml);
            const kl_stats *stats=kl_get_stats(bsml);
            fprintf(stderr,"BSML before callback: %u imports bound, %u missing\n",stats->imports_bound,stats->imports_missing);
            assert(!stats->imports_missing);
        }
    }
    void (*data_dir)(const char *) = kl_sym(image, "il2cpp_set_data_dir");
    void (*config_dir)(const char *) = kl_sym(image, "il2cpp_set_config_dir");
    void *(*domain_get)(void) = kl_sym(image, "il2cpp_domain_get");
    const void **(*assemblies)(void *, size_t *) = kl_sym(image, "il2cpp_domain_get_assemblies");
    assert(data_dir && config_dir && domain_get && assemblies);
    data_dir(argv[2]); config_dir(argv[2]);
    if(getenv("SC2_1453_SCENE_ICALL")) {
        // Unity normally registers this while booting. This isolated host test
        // supplies its independently measured registration without starting
        // Unity's graphics/window subsystem. Production registration is untested.
        FILE *file=fopen(getenv("SC2_1453_SCENE_ICALL"),"r");assert(file);
        unsigned long long offset;char name[256];assert(fscanf(file,"%llx %255s",&offset,name)==2);fclose(file);
        kl_image *unity=kl_find_image("libunity.so");assert(unity && offset<kl_span(unity));
        void (*add)(const char*,void*)=kl_sym(image,"il2cpp_add_internal_call");assert(add);
        add(name,(char*)kl_base(unity)+offset);
    }
    void *original = kl_sym(image, "il2cpp_init");
    assert(original);
    assert(!kl_scotland2_interpose(NULL, "il2cpp_init", original));
    assert(!kl_scotland2_interpose(image, "il2cpp_domain_get", original));
    // Resolve through the same guest dlsym seam Unity uses.
    void *(*symbol)(void *, const char *) = kl_shim_lookup("dlsym");
    void (*init)(const char *) = symbol(image, "il2cpp_init");
    assert(init && (void *)init != original);
    init("klepton-scotland2");
    void *domain = domain_get();
    assert(domain); // Unity 2022 init has a void API; read domain_get separately.
    size_t count = 0;
    assert(assemblies(domain, &count) && count > 0);
    assert(symbol(image,"il2cpp_init")==original);
    if(getenv("SC2_1453_PROBE")) {
        kl_image *probe=kl_scotland2_open_module(getenv("SC2_1453_PROBE"),2);
        if(!probe){fprintf(stderr,"1453 guest fixture: %s\n",kl_error());return 1;}
        int (*sdk)(void**,size_t)=kl_sym(probe,"sc2_test_1453_sdk");
        int (*registration)(void)=kl_sym(probe,"sc2_test_1453_registration");assert(sdk&&registration);
        void *resolved[3]={0};int result=sdk(resolved,3);
        fprintf(stderr,"SDK_1453_RESULT %d\n",result);if(result)return 1;
        FILE *file=fopen(getenv("SC2_1453_SDK_PROFILE"),"r");assert(file);
        unsigned long long offset;char name[128];unsigned checked=0;
        while(fscanf(file,"%llx %127s",&offset,name)==2){
            if(checked>=3 || resolved[checked]!=(char*)kl_base(image)+offset){
                fprintf(stderr,"SDK target mismatch: %s\n",name);return 1;
            }
            ++checked;
        }
        assert(feof(file)&&checked==3);fclose(file);
        result=registration();fprintf(stderr,"REGISTRATION_1453_RESULT %d\n",result);if(result)return 1;
        verify_mod_hooks(image,getenv("SC2_1453_CT_PROFILE"),"libcustom-types.so",5);
        if(getenv("SC2_1453_EARLY")) {
            verify_mod_hooks(image,getenv("SC2_1453_LAPIZ_PROFILE"),"liblapiz.so",26);
            verify_mod_hooks(image,getenv("SC2_1453_BSML_PROFILE"),"libbsml.so",14);
            verify_mod_hooks(kl_find_image("libunity.so"),getenv("SC2_1453_SCENE_PROFILE"),"libsc2_scene.so",1);
            puts("EARLY_MODS_1453_OK original Scotland2 setup/load; 26 Lapiz and 14 BSML hooks verified; scene hook armed with measured host icall registration, not fired.");
        }
        printf("SDK_REGISTRATION_1453_OK %zu domain assemblies; SDK targets and five custom-type hooks match measured addresses; actual mod types and object construction verified.\n",count);
        return 0;
    }
    if(getenv("SC2_INIT_ONLY")) {
        printf("GAME_IL2CPP_INIT_OK %zu assemblies through original Scotland2 phase bridge; no Unity startup or mod load callbacks exercised.\n",count);
        return 0;
    }
    if(getenv("SC2_REMAINING_CORE")) {
        const char *names[]={"datakeeper","unicode"};
        const char *libs[]={"libdatakeeper.so","libunicode.so"};
        const char *checks[]={"sc2_test_datakeeper_files","sc2_test_unicode_raster"};
        const unsigned counts[]={4,4};
        for(unsigned i=0;i<sizeof names/sizeof names[0];++i){
            char path[4096];snprintf(path,sizeof path,"%s/test-only/%s",getenv("SC2_REMAINING_META"),libs[i]);
            kl_image *mod=kl_find_image(libs[i]);
            if(!mod){mod=kl_scotland2_open_module(path,2);if(!mod){fprintf(stderr,"Remaining mod %s: %s\n",names[i],kl_error());return 1;}
                typedef struct {const char*id,*version;uint64_t tag;} info_t;info_t info={0};
                void (*setup)(info_t*)=kl_sym(mod,"setup");assert(setup);setup(&info);
            }
            assert(!kl_get_stats(mod)->imports_missing);
            if(i){void (*late)(void)=kl_sym(mod,"late_load");assert(late);late();}
            snprintf(path,sizeof path,"%s/android-%s/hook-profile.txt",getenv("SC2_REMAINING_BUILD"),names[i]);
            verify_mod_hooks(image,path,libs[i],counts[i]);
            if(i==1){int (*api)(void)=kl_sym(mod,"sc2_test_unicode_upload_api");assert(api&&!api());}
            int (*check)(void)=kl_sym(mod,checks[i]);assert(check);int result=check();
            fprintf(stderr,"REMAINING_CORE_HOST %s imports=%u result=%d\n",names[i],kl_get_stats(mod)->imports_bound,result);if(result)return 1;
        }
        puts("REMAINING_CORE_HOST_OK original save backup/restore and actual Android-to-Apple glyph rendering; Fake ID excluded");return 0;
    }
    if (getenv("SC2_LAPIZ_CATALOG")) {
        puts("LAPIZ_EARLY_CALLBACK_OK actual Scotland2 dispatched Lapiz load after IL2CPP initialization.");
    }
    if (getenv("SC2_BSML_CATALOG")) {
        verify_mod_hooks(image,getenv("SC2_BSML_PROFILE"),"libbsml.so",13);
        void *(*open)(const char*,int)=kl_shim_lookup("dlopen");
        kl_image *probe=open(getenv("SC2_TYPES_PROBE_PATH"),2);
        assert(probe && !kl_get_stats(probe)->imports_missing);
        int (*run)(void)=kl_sym(probe,"sc2_test_bsml_types");assert(run);
        int result=run();fprintf(stderr,"BSML type consumer result: %d\n",result);assert(!result);
        char config[4096];snprintf(config,sizeof config,"%s/ModData/com.beatgames.beatsaber/Configs/bsml.json",kl_jni_files_dir());
        FILE *file=fopen(config,"r");assert(file);char data[4096];size_t n=fread(data,1,sizeof data-1,file);data[n]=0;fclose(file);
        assert(strstr(data,"hiddenTabs"));
        int (*mkpath)(const char*,size_t)=kl_sym(probe,"sc2_test_bshook_mkpath");assert(mkpath);
        const char bounded[]="/sdcard/ModData/filesystem probe/bounded-suffix";
        assert(!mkpath(bounded,strlen(bounded)-strlen("-suffix")));
        char host[4096];struct stat info;
        snprintf(host,sizeof host,"%s/ModData/filesystem probe/bounded",kl_jni_files_dir());
        assert(!stat(host,&info)&&S_ISDIR(info.st_mode));
        const char alias[]="/storage/emulated/0/ModData/filesystem probe/bounded";
        assert(!mkpath(alias,strlen(alias))); // existing directory and Android alias
        snprintf(host,sizeof host,"%s/ModData/filesystem probe/blocker",kl_jni_files_dir());
        file=fopen(host,"w");assert(file);fclose(file);
        const char blocked[]="/sdcard/ModData/filesystem probe/blocker/child";
        assert(mkpath(blocked,strlen(blocked))!=0);
        assert(mkpath("",0)!=0);
        const char embedded[]="/sdcard/ModData/invalid\0suffix";
        assert(mkpath(embedded,sizeof embedded-1)!=0);
        puts("BSHOOK_FILESYSTEM_OK actual mkpath entry handles nested directories, spaces, bounded string views, aliases and existing directories; rejects files, empty paths and embedded NULs.");
        puts("BSML_EARLY_OK actual early callback, thirteen installed game targets, 61 registered types and real configuration file written; no late Unity callback run on host.");
    }
    if(getenv("SC2_BSML_LIFECYCLE_PATH")) {
        kl_image *consumer=kl_scotland2_open_module(getenv("SC2_BSML_LIFECYCLE_PATH"),2);
        assert(consumer && !kl_get_stats(consumer)->imports_missing);
        assert(kl_sym(consumer,"sc2_test_bsml_late"));
        // Engine object calls are deliberately deferred to real device startup.
        puts("BSML_LIFECYCLE_CONSUMER_READY imports resolved; Unity calls deferred to device.");
    }
    if (getenv("SC2_SEARCH_MODULE")) {
        const char *songcore=getenv("SC2_SEARCH_SONGCORE");assert(songcore);
        kl_image *song=kl_scotland2_open_module(songcore,2);assert(song && !kl_get_stats(song)->imports_missing);
        kl_image *search=kl_scotland2_open_module(getenv("SC2_SEARCH_MODULE"),2);
        if(!search){fprintf(stderr,"SEARCH_LOAD_FAILED %s\n",kl_error());return 1;}
        const kl_stats *stats=kl_get_stats(search);
        fprintf(stderr,"SEARCH_IMPORTS bound=%u missing=%u\n",stats->imports_bound,stats->imports_missing);
        assert(!stats->imports_missing && kl_sym(search,"setup") && kl_sym(search,"late_load"));
        typedef struct {const char *id,*version;uint64_t version_long;} mod_info;
        void (*setup)(mod_info*)=kl_sym(search,"setup");mod_info info={0};setup(&info);
        assert(info.id && info.version && !strcmp(info.id,getenv("SC2_SEARCH_ID")) && !strcmp(info.version,getenv("SC2_SEARCH_VERSION")));
        fprintf(stderr,"SEARCH_LOAD_SETUP_OK %s %s; original constructors/setup, real game IL2CPP and strict import resolution. No late menu callback or network call.\n",info.id,info.version);
        sleep(1);return 0;
    }
    if(getenv("SC2_SONGCORE_PATH")) {
        // The strict module open checks imports before running constructors.
        // SongCore's late callback needs Unity engine objects and is device-only.
        kl_image *song=kl_scotland2_open_module(getenv("SC2_SONGCORE_PATH"),2);
        if(!song){fprintf(stderr,"SongCore load: %s\n",kl_error());return 1;}
        const kl_stats *stats=kl_get_stats(song);
        fprintf(stderr,"SongCore: %u imports bound, %u missing\n",stats->imports_bound,stats->imports_missing);
        if(stats->imports_missing) {
            unsigned n=0;const char *const *missing=kl_missing_imports(song,&n);
            for(unsigned i=0;i<n;++i)fprintf(stderr,"Missing SongCore import: %s\n",missing[i]);
            return 1;
        }
        typedef struct {const char *id,*version;uint64_t version_long;} mod_info;
        void (*setup)(mod_info*)=kl_sym(song,"setup");assert(setup);
        mod_info info={0};setup(&info);
        assert(info.id&&!strcmp(info.id,"SongCore")&&info.version&&!strcmp(info.version,"1.1.24"));
        char config[4096];snprintf(config,sizeof config,"%s/ModData/com.beatgames.beatsaber/Configs/SongCore.json",kl_jni_files_dir());
        FILE *file=fopen(config,"r");assert(file);fclose(file);
        kl_image *types=kl_scotland2_open_module(getenv("SC2_TYPES_PROBE_PATH"),2);
        int (*register_types)(void)=types?kl_sym(types,"sc2_test_songcore_types"):NULL;assert(register_types);
        int type_result=register_types();fprintf(stderr,"SongCore registration consumer result: %d\n",type_result);assert(!type_result);
        kl_image *service=kl_scotland2_open_module(getenv("SC2_SONGCORE_LIFECYCLE_PATH"),2);
        assert(service && !kl_get_stats(service)->imports_missing);
        assert(kl_sym(service,"sc2_test_songcore_lifecycle"));
        void *(*generic_target)(void)=kl_sym(service,"sc2_songcore_generic_target");assert(generic_target);
        void *cache_add=generic_target();assert(cache_add);
        uintptr_t cache_offset=(uintptr_t)cache_add-(uintptr_t)kl_base(image);
        fprintf(stderr,"SONGCORE_GENERIC_TARGET actual LRUCache<String,Sprite>.Add +0x%zx word=0x%08x\n",(size_t)cache_offset,*(uint32_t*)cache_add);
        assert(cache_offset==0x3708fb8);
        puts("SONGCORE_GENERIC_TARGET_OK original runtime generic lookup matches the independently measured physical target.");
        puts("SONGCORE_LIFECYCLE_CONSUMER_READY imports resolved; service/scan observation deferred to device.");
        puts("SONGCORE_TYPES_OK 23 actual SongCore classes registered and resolved through game name/type APIs; no duplicate registration.");
        puts("SONGCORE_LOAD_SETUP_OK actual source DSO constructors and setup executed, imports resolved and configuration created. Not a late callback or song-playback test.");
    }
    if(getenv("SC2_DOWNLOAD_LIBRARIES")) {
        const char *names[]={"libweb-utils.so","libbeatsaverplusplus.so","libzip.so","libsongdetails.so"};
        for(unsigned i=0;i<sizeof names/sizeof *names;++i) {
            kl_image *library=kl_find_image(names[i]);assert(library);
            const kl_stats *stats=kl_get_stats(library);
            fprintf(stderr,"DOWNLOAD_LIBRARY %s: %u imports bound, %u missing\n",names[i],stats->imports_bound,stats->imports_missing);
            assert(!stats->imports_missing);
        }
        kl_image *zip=kl_find_image("libzip.so");
        void *(*open_zip)(const char*,int,int*)=kl_sym(zip,"zip_open");
        void *(*open_entry)(void*,const char*,uint32_t)=kl_sym(zip,"zip_fopen");
        int64_t (*read_entry)(void*,void*,uint64_t)=kl_sym(zip,"zip_fread");
        int (*close_entry)(void*)=kl_sym(zip,"zip_fclose");
        int (*close_zip)(void*)=kl_sym(zip,"zip_close");
        assert(open_zip&&open_entry&&read_entry&&close_entry&&close_zip);
        int zip_error=0;void *archive=open_zip(getenv("SC2_TEST_ZIP_PATH"),0,&zip_error);assert(archive);
        void *entry=open_entry(archive,"payload.txt",0);assert(entry);
        char data[4096];int64_t size=read_entry(entry,data,sizeof data);const char expected[]="SCOTLAND2_REAL_ZIP_DEFLATE\n";
        assert(size==(int64_t)((sizeof expected-1)*100));
        for(unsigned j=0;j<100;++j)assert(!memcmp(data+j*(sizeof expected-1),expected,sizeof expected-1));
        assert(read_entry(entry,data,sizeof data)==0 && !close_entry(entry) && !close_zip(archive));
        puts("DOWNLOAD_ZIP_DEFLATE_OK actual released libzip read and decompressed a known ZIP entry.");
        kl_image *network=kl_scotland2_open_module(getenv("SC2_DOWNLOAD_PROBE_PATH"),2);
        assert(network && !kl_get_stats(network)->imports_missing);
        int (*request)(const char*)=kl_sym(network,"sc2_test_webutils");assert(request);
        if(getenv("SC2_TEST_WEBUTILS_URL")) {
            int (*safety)(const char*)=kl_sym(network,"sc2_test_webutils_callback_safety");assert(safety && !safety(getenv("SC2_TEST_WEBUTILS_URL")));
            puts("WEBUTILS_CALLBACK_SAFETY_OK invalid URLs fail and progress callback exceptions abort the request without unwinding through curl");
            int network_result=request(getenv("SC2_TEST_WEBUTILS_URL"));
            fprintf(stderr,"WebUtils network consumer result: %d\n",network_result);if(network_result)return 1;
            puts("DOWNLOAD_HTTPS_OK original WebUtils performed verified HTTPS and returned a nonempty successful response.");
        }
        if(getenv("SC2_TEST_WEBUTILS_BAD_TLS")) {
            int (*reject_bad_tls)(const char*)=kl_sym(network,"sc2_test_webutils_reject_bad_tls");assert(reject_bad_tls);
            if(reject_bad_tls(getenv("SC2_TEST_WEBUTILS_BAD_TLS")))return 1;
            puts("DOWNLOAD_BAD_TLS_REJECTED_OK original WebUtils/curl rejects an untrusted HTTPS certificate even for callers using its old default option.");
        }
        if(getenv("SC2_TEST_BAD_ZIP_PATH")) {
            int (*extract)(const char*,size_t,const char*)=kl_sym(network,"sc2_test_beatsaver_safe_extract");assert(extract);check_safe_extract(extract,"BEATSAVER");
        }
        kl_image *details=kl_find_image("libsongdetails.so");
        int (*hashes)(void)=kl_sym(details,"sc2_test_songdetails_hashes");assert(hashes && !hashes());
        int (*payloads)(void)=kl_sym(details,"sc2_test_songdetails_payloads");assert(payloads && !payloads());
        puts("SONGDETAILS_PAYLOAD_BOUNDS_OK real protobuf count/hash validation, valid database, malformed replacement preserves state, invalid Brotli rejected");
        puts("SONGDETAILS_HASH_BOUNDS_OK malformed/short hashes rejected by original library helper");
        int (*tls)(const char*,int)=kl_sym(details,"sc2_test_songdetails_tls");assert(tls);
        if(getenv("SC2_TEST_WEBUTILS_URL")){assert(!tls(getenv("SC2_TEST_WEBUTILS_URL"),1));puts("SONGDETAILS_HTTPS_OK original library verified HTTPS");}
        if(getenv("SC2_TEST_WEBUTILS_BAD_TLS")){assert(!tls(getenv("SC2_TEST_WEBUTILS_BAD_TLS"),0));puts("SONGDETAILS_BAD_TLS_REJECTED_OK original library rejected untrusted certificate");}
        puts("DOWNLOAD_LIBRARIES_LOAD_OK original WebUtils/BeatSaverPlusPlus/zip/SongDetails constructors and imports passed; network checks reported separately.");
    }
    if(getenv("SC2_TEST_METACORE")) {
        kl_image *meta=kl_find_image("libmetacore.so");assert(meta && !kl_get_stats(meta)->imports_missing);
        kl_image *consumer=kl_scotland2_open_module(getenv("SC2_TYPES_PROBE_PATH"),2);assert(consumer);
        int (*types)(void)=kl_sym(consumer,"sc2_test_metacore_types");assert(types && !types());
        int (*install)(void)=kl_sym(meta,"sc2_test_metacore_install_hooks");assert(install && !install());
        verify_mod_hooks(image,getenv("SC2_METACORE_PROFILE"),"libmetacore.so",36);
        void *abort_gate=kl_shim_lookup("abort");assert(kl_aot_original(abort_gate));
        const char *owner=kl_addr_image(kl_aot_current(abort_gate),NULL);assert(owner && !strcmp(owner,"libmetacore.so"));
        kl_image *unity=kl_find_image("libunity.so");assert(unity);
        void *delete_step=(char*)kl_base(unity)+0x511008;
        owner=kl_addr_image(kl_aot_current(delete_step),NULL);assert(owner && !strcmp(owner,"libmetacore.so"));
        fprintf(stderr,"METACORE_HOST_HOOKS_OK %u imports, four registered types, 36 game hooks plus original native abort and Unity deletion installers; no original late GameObject callback run.\n",kl_get_stats(meta)->imports_bound);
    }
    if(getenv("SC2_PLAYLISTCORE_PATH")) {
        kl_image *playlist=kl_scotland2_open_module(getenv("SC2_PLAYLISTCORE_PATH"),2);
        if(!playlist){fprintf(stderr,"PlaylistCore strict load: %s\n",kl_error());return 1;}
        assert(!kl_get_stats(playlist)->imports_missing);
        typedef struct {const char *id,*version;uint64_t version_long;} mod_info;
        void (*setup)(mod_info*)=kl_sym(playlist,"setup");assert(setup);
        mod_info info={0};setup(&info);
        assert(info.id&&!strcmp(info.id,"PlaylistCore")&&info.version&&!strcmp(info.version,"1.5.5"));
        kl_image *consumer=kl_scotland2_open_module(getenv("SC2_TYPES_PROBE_PATH"),2);assert(consumer);
        int (*types)(void)=kl_sym(consumer,"sc2_test_playlistcore_types");assert(types && !types());
        void (*install)(void)=kl_sym(playlist,"sc2_test_playlistcore_install_hooks");assert(install);install();
        FILE *profile=fopen(getenv("SC2_PLAYLISTCORE_PROFILE"),"r");assert(profile);
        unsigned long long offset;unsigned word,count=0;char name[128];
        while(fscanf(profile,"%llx %x %127s",&offset,&word,name)==3) {
            void *target=(char*)kl_base(image)+offset;uint32_t original=0;int found=0;
            assert(!kl_aot_entry_word(target,&original)&&original==word);
            // InstallOrigHook deliberately sits below the current head hook.
            for(int layer=0;target && layer<=8;++layer) {
                const char *owner=kl_addr_image(kl_aot_current(target),NULL);
                if(owner&&!strcmp(owner,"libplaylistcore.so")){found=1;break;}
                target=kl_aot_original(target);
            }
            if(!found)fprintf(stderr,"Missing PlaylistCore hook layer: %s\n",name);
            assert(found);++count;
        }
        assert(feof(profile)&&count==8);fclose(profile);
        fprintf(stderr,"PLAYLISTCORE_HOST_HOOKS_OK %u imports, original setup, five registered types and all eight game hook layers; actual late UI callback deferred to device.\n",kl_get_stats(playlist)->imports_bound);
#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
        assert(!kl_scotland2_verify_mod_hooks(getenv("SC2_PLAYLISTCORE_PROFILE"),"libplaylistcore.so",8));
        assert(!kl_scotland2_verify_mod_hooks(getenv("SC2_METACORE_PROFILE"),"libmetacore.so",36));
        assert(!kl_scotland2_verify_mod_hooks(getenv("SC2_BSML_PROFILE"),"libbsml.so",13));
        assert(!kl_scotland2_verify_mod_hooks("build/scotland2/android-lapiz/hook-profile.txt","liblapiz.so",31));
        puts("MOD_SHARED_HOOK_CHAIN_OK original SDK links preserve BSML, MetaCore and PlaylistCore together at the shared restart target and both PlaylistCore original hook layers.");
#endif
    }
    if(getenv("SC2_SONGDOWNLOADER_PATH")) {
        kl_image *browser=kl_scotland2_open_module(getenv("SC2_SONGDOWNLOADER_PATH"),2);
        if(!browser){fprintf(stderr,"SongDownloader strict load: %s\n",kl_error());return 1;}
        assert(!kl_get_stats(browser)->imports_missing);
        typedef struct {const char *id,*version;uint64_t version_long;} mod_info;
        void (*setup)(mod_info*)=kl_sym(browser,"setup");assert(setup);
        mod_info info={0};setup(&info);
        assert(info.id&&!strcmp(info.id,"SongDownloader")&&info.version&&!strcmp(info.version,"0.8.8"));
        int (*zip)(const char*)=kl_sym(browser,"sc2_test_songdownloader_zip");assert(zip&&!zip(getenv("SC2_TEST_ZIP_PATH")));
        if(getenv("SC2_TEST_BAD_ZIP_PATH")) {
            int (*extract)(const char*,size_t,const char*)=kl_sym(browser,"sc2_test_songdownloader_safe_extract");assert(extract);check_safe_extract(extract,"SONGDOWNLOADER");
        }
        puts("SONGDOWNLOADER_ZIP_OK original browser's own pinned kuba zip read and decompressed fixture.");
        if(getenv("SC2_TEST_WEBUTILS_URL")) {
            int (*request)(const char*)=kl_sym(browser,"sc2_test_songdownloader_request");assert(request);
            if(request(getenv("SC2_TEST_WEBUTILS_URL")))return 1;
            int (*api)(void)=kl_sym(browser,"sc2_test_songdownloader_api");assert(api);
            if(api())return 1;
            int (*callback_safety)(const char*)=kl_sym(browser,"sc2_test_songdownloader_callback_safety");assert(callback_safety && !callback_safety(getenv("SC2_TEST_WEBUTILS_URL")));
            puts("SONGDOWNLOADER_CALLBACK_SAFETY_OK progress callback failure becomes a failed request");
            puts("SONGDOWNLOADER_API_OK actual browser curl verified HTTPS, fetched and parsed a nonempty BeatSaver page through original API code.");
        }
        if(getenv("SC2_TEST_SONGDOWNLOADER_BAD_TLS")) {
            int (*reject)(const char*)=kl_sym(browser,"sc2_test_songdownloader_bad_tls");assert(reject);
            if(reject(getenv("SC2_TEST_SONGDOWNLOADER_BAD_TLS")))return 1;
            puts("SONGDOWNLOADER_BAD_TLS_REJECTED_OK original browser's HTTPS rejected an untrusted test certificate.");
        }
        kl_image *types=kl_scotland2_open_module(getenv("SC2_TYPES_PROBE_PATH"),2);assert(types);
        int (*register_types)(void)=kl_sym(types,"sc2_test_songdownloader_types");assert(register_types&&!register_types());
        fprintf(stderr,"SONGDOWNLOADER_LOAD_SETUP_OK %u imports, actual setup, four registered types; original late menu callback deferred to physical headset.\n",kl_get_stats(browser)->imports_bound);
    }
    const char *customtypes=getenv("KL_SCOTLAND2_CUSTOMTYPES_PATH");
    if (customtypes && *customtypes) {
        int (*malloc_info)(int,void *)=kl_shim_lookup("malloc_info");
        int *(*guest_errno)(void)=kl_shim_lookup("__errno");
        assert(malloc_info && guest_errno);
        assert(malloc_info(1,NULL)==-1 && *guest_errno()==22);
        assert(malloc_info(0,NULL)==-1 && *guest_errno()==95);
        void *(*open)(const char *,int)=kl_shim_lookup("dlopen");
        kl_image *ct=open(customtypes,2);
        if (!ct) { fprintf(stderr,"custom-types load failed: %s\n",kl_error()); return 1; }
        const kl_stats *stats=kl_get_stats(ct);
        fprintf(stderr,"custom-types: %u imports bound, %u missing\n",stats->imports_bound,stats->imports_missing);
        if (stats->imports_missing) {
            unsigned n=0;const char *const *missing=kl_missing_imports(ct,&n);
            for (unsigned i=0;i<n;++i) fprintf(stderr,"Missing custom-types import: %s\n",missing[i]);
            return 1;
        }
        int (*delegate_count)(void)=kl_sym(ct,"_ZN12custom_types18get_delegate_countEv");
        assert(delegate_count && delegate_count()==0 && delegate_count()==1);
        puts("CUSTOMTYPES_CODE_OK actual rebuilt delegate registry executed; type-registration hooks not yet installed.");
        if (getenv("KL_SCOTLAND2_CUSTOMTYPES_REGISTER")) {
            const char *unity_path=getenv("KL_SCOTLAND2_CUSTOMTYPES_UNITY");assert(unity_path);
            kl_image *unity=kl_load_auto(unity_path);
            if (!unity) { fprintf(stderr,"Unity inspection image: %s\n",kl_error()); return 1; }
            if (kl_get_stats(unity)->imports_missing) {
                unsigned n=0;const char *const *missing=kl_missing_imports(unity,&n);
                for (unsigned i=0;i<n;++i) fprintf(stderr,"Unity inspection missing import: %s\n",missing[i]);
                // Existing Unity anti-debug import; this host test loads its
                // code for the prepared registration hook, not Unity startup.
                assert(n==1 && !strcmp(missing[0],"ptrace"));
            }
            kl_register_image("libunity.so",unity);
        }
        kl_image *bs=kl_find_image("libbeatsaber-hook.so");assert(bs);
        void (*sdk_init)(void)=kl_sym(bs,"_ZN16il2cpp_functions4InitEv");assert(sdk_init);
        sdk_init();
        const char *names[]={"_ZN16il2cpp_functions27il2cpp_Class_FromIl2CppTypeE",
            "_ZN16il2cpp_functions43il2cpp_GlobalMetadata_GetTypeInfoFromHandleE",
            "_ZN16il2cpp_functions56il2cpp_GlobalMetadata_GetTypeInfoFromTypeDefinitionIndexE",
            "_ZN16il2cpp_functions17il2cpp_Class_InitE"};
        for (unsigned i=0;i<sizeof names/sizeof *names;++i) {
            void **entry=kl_sym(bs,names[i]);assert(entry && *entry);
            fprintf(stderr,"CUSTOMTYPES_TARGET %s +0x%zx word=0x%08x\n",names[i],
                (size_t)((uintptr_t)*entry-(uintptr_t)kl_base(image)),*(uint32_t *)*entry);
        }
        puts("CUSTOMTYPES_DISCOVERY_OK actual beatsaber-hook initialized and resolved the four primary registration hook targets.");
        if (getenv("KL_SCOTLAND2_CUSTOMTYPES_REGISTER")) {
            for (unsigned i=0;i<sizeof names/sizeof *names;++i) {
                void **entry=kl_sym(bs,names[i]);assert(kl_aot_original(*entry));
            }
            kl_image *probe=open(getenv("SC2_CT_REGISTRATION_PROBE"),2);
            assert(probe && !kl_get_stats(probe)->imports_missing);
            int (*run)(void)=kl_sym(probe,"sc2_test_customtypes_register");assert(run);
            int result=run();
            fprintf(stderr,"custom-types registration result: %d\n",result);assert(!result);
            puts("CUSTOMTYPES_REGISTRATION_OK actual built-in classes registered once, found by game name/type APIs and instantiated through IL2CPP.");
        }
    }
    printf("PASS: actual IL2CPP initialized through Scotland2's early-phase bridge; %zu assemblies, domain %p.\n", count, domain);
    return 0;
}
