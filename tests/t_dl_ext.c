// Exercise the guest-facing symbol, with no guest binaries or constructors.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "guest/kl_driver.h"

int main(void) {
    const kl_target target = {.name = "dl-ext-test", .kind = KL_GUEST_UNITY};
    kl_driver_init(&target, NULL, 0);
    void *(*open_ext)(const char *, int, const void *) =
        kl_shim_lookup("android_dlopen_ext");
    void *(*open_plain)(const char *, int) = kl_shim_lookup("dlopen");
    void *(*symbol)(void *, const char *) = kl_shim_lookup("dlsym");
    const char *(*error)(void) = kl_shim_lookup("dlerror");
    int (*close_lib)(void *) = kl_shim_lookup("dlclose");
    assert(open_ext && open_plain && symbol && error && close_lib);
    (void)error();
    assert(open_ext(NULL, 2, NULL) == open_plain(NULL, 2));
    void *h = open_ext("libdl.so", 2, NULL);
    assert(h && h == open_plain("libdl.so", 2));
    assert(symbol(h, "android_dlopen_ext") == (void *)open_ext);
    // Only the first field may be read when no extension flag is requested.
    uint64_t extflags = 0;
    assert(open_ext("libdl.so", 2, &extflags) == h);
    assert(!error() && close_lib(h) == 0);

    // All extension bits, including unknown high bits, must fail honestly.
    for (unsigned bit = 0; bit < 64; bit++) {
        extflags = UINT64_C(1) << bit;
        assert(!open_ext("libdl.so", 2, &extflags));
        const char *e = error();
        assert(e && strstr(e, "unsupported extension flags"));
        assert(!error());
    }
    extflags = 0;
    assert(!open_ext("/nonexistent/klepton-dl-ext-test.so", 2, &extflags));
    const char *e = error();
    assert(e && strstr(e, "not found"));
    assert(!error());
    puts("PASS: android_dlopen_ext delegation, symbol lookup and explicit unsupported-flag errors.");
    return 0;
}
