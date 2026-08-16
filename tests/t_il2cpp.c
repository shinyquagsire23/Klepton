// libil2cpp.so loads with 0 unresolved imports and il2cpp_init
// returns. This drives Unity's IL2CPP runtime far enough to parse global-metadata.dat
// and stand up its type system.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/klepton.h"
#include "../runtime/guest/kl_il2cpp.h"

int main(int argc, char **argv) {
    const char *lib  = argc > 1 ? argv[1] : "beatsaber/lib/arm64-v8a/libil2cpp.so";
    const char *data = argc > 2 ? argv[2] : "beatsaber/assets/bin/Data";

    kl_thread_init();
    printf("=== klepton: %s ===\n", lib);

    kl_image *img = kl_load(lib);
    if (!img) { printf("load failed: %s\n", kl_error()); return 1; }
    const kl_stats *st = kl_get_stats(img);
    printf("  %u relocations, %u imports bound, %u unresolved\n",
           st->relative + st->abs64 + st->glob_dat + st->jump_slot,
           st->imports_bound, st->imports_missing);
    if (st->imports_missing) { printf("  !! unresolved imports remain\n"); return 1; }

    kl_set_library_path("beatsaber/lib/arm64-v8a");
    kl_register_image(lib, img);

    printf("  running DT_INIT_ARRAY…\n"); fflush(stdout);
    kl_run_init(img);
    printf("  init OK\n");

    void (*set_data_dir)(const char *)   = kl_sym(img, "il2cpp_set_data_dir");
    void (*set_config_dir)(const char *) = kl_sym(img, "il2cpp_set_config_dir");
    void *(*il2cpp_init)(const char *)   = kl_sym(img, "il2cpp_init");
    void *(*domain_get)(void)            = kl_sym(img, "il2cpp_domain_get");
    void *(*get_assemblies)(void *, size_t *) = kl_sym(img, "il2cpp_domain_get_assemblies");

    if (!il2cpp_init) { printf("  !! il2cpp_init not exported\n"); return 1; }
    if (set_data_dir)   { printf("  il2cpp_set_data_dir(\"%s\")\n", data); set_data_dir(data); }
    if (set_config_dir) set_config_dir(data);

    printf("\n  calling il2cpp_init(\"klepton\")…\n"); fflush(stdout);
    void *domain = il2cpp_init("klepton");
    printf("  il2cpp_init returned %p\n", domain);
    if (!domain) { printf("  !! il2cpp_init failed\n"); return 1; }

    if (domain_get) printf("  il2cpp_domain_get() = %p\n", domain_get());
    if (get_assemblies) {
        size_t n = 0;
        void *as = get_assemblies(domain, &n);
        printf("  loaded assemblies: %zu (array %p)\n", n, as);
    }

    printf("\n=== EXIT CRITERION MET: IL2CPP runtime initialised ===\n");

    // KL_RESOLVE=0xoff,0xoff,... — init the method-name resolver and print
    // the managed method containing each libil2cpp image offset. Built to
    // read static disassembly call targets (bl 0x...) without a live boot.
    const char *rz = getenv("KL_RESOLVE");
    if (rz) {
        char meta[1024];
        snprintf(meta, sizeof meta, "%s/Metadata/global-metadata.dat", data);
        if (kl_il2cpp_resolver_init(img, meta)) {
            printf("  resolver up: %u methods\n", kl_il2cpp_method_count());
            char *copy = strdup(rz), *tok = strtok(copy, ", ");
            while (tok) {
                uint64_t off = strtoull(tok, NULL, 0);
                const char *m = kl_il2cpp_method_at((uint8_t *)kl_base(img) + off);
                printf("  libil2cpp+%#llx: %s\n", (unsigned long long)off,
                       m ? m : "(not a known method)");
                tok = strtok(NULL, ", ");
            }
            free(copy);
        } else printf("  !! resolver init failed\n");
    }
    return 0;
}
