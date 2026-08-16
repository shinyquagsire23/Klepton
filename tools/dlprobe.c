// The P2 gate's whole payload: does *this platform's dyld* accept a
// klepton-ld-emitted Mach-O at all?
//
// Deliberately links nothing but libSystem, so a failure is unambiguous. If the
// runtime were linked in, a dyld rejection and a shim bug would look the same,
// and rung 2's entire value is that it catches malformed Mach-O.
// The roundtrip through the loaded image is t_opus's job, on whichever rung can
// build the runtime.
#include <stdio.h>
#include <dlfcn.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: dlprobe <path.dylib>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
    printf("dlopen OK: %p\n", h);
    return 0;
}
