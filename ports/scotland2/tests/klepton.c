// Real QMOD library, translated and ad-hoc signed, opened by Scotland2's core.
#include "apple-platform.h"
#include "klepton.h"
#include "kl_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int owns(void *handle, const void *symbol, const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *owner = kl_addr_image(symbol, NULL);
    return handle == kl_find_image(base) && owner && !strcmp(owner, base);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    kl_target target = {.name="beatsaber140", .kind=KL_GUEST_UNITY};
    kl_driver_init(&target, NULL, 0);
    sc2_platform_api api = {
        .open=kl_shim_lookup("dlopen"), .symbol=kl_shim_lookup("dlsym"),
        .error=kl_shim_lookup("dlerror"), .close=kl_shim_lookup("dlclose"), .owns=owns,
    };
    assert(sc2_apple_configure(&api, argv[1], "com.beatgames.beatsaber", NULL) == 0);
    if (sc2_apple_open_libraries() != 0) {
        fprintf(stderr, "Scotland2 failed: %s\n", sc2_apple_error());
        return 1;
    }
    kl_image *image = kl_find_image("libtinyxml2.so");
    assert(image);
    const kl_stats *stats = kl_get_stats(image);
    printf("tinyxml2 import inventory: %u bound, %u missing\n", stats->imports_bound, stats->imports_missing);
    assert(stats->imports_missing == 0);
    bool (*to_int)(const char *, int *) = kl_sym(image, "_ZN8tinyxml27XMLUtil5ToIntEPKcPi");
    assert(to_int);
    int value = 0;
    assert(to_int("1234", &value) && value == 1234);
    assert(!to_int("not-a-number", &value));
    puts("PASS: actual tinyxml2 QMOD library opened by Scotland2 and executed through signed Klepton translation.");
    return 0;
}
