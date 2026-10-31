// The x18 data window's two-entry literal-pool rule, and its scope.
// `make x18pool`, in `make check`.
//
// kl_x18.c refuses an x18 site when its ±16-word window holds two or more
// "unallocated" words: that is how constant tables in .text with no symbol over
// them (BoringSSL, CRYPTOGAMS) are kept from being patched. Robo Recall:
// Unplugged's PhysX sits next to two-entry GCC literal pools, so six real sites
// were refused and a capsule sweep faulted. For that target alone a lone
// adjacent pair counts as a pool. This checks both halves of that sentence on
// real words:
//
//   1. a PhysX window — `ldr x18, [x0]` at the top of
//      Gu::SweepGeomGeom<CapsuleV, CapsuleV>, two words after the pool
//      `b ...; 100.0f; 100.0f` (Tales from the Galaxy's Edge's libUE4.so
//      +0x825c748; the same engine code as Robo Recall's) — is patched under
//      the Robo Recall target and refused, exactly as before, everywhere else;
//   2. one word of BoringSSL's P-256 table (Beat Saber 1.40's libcrypto.so
//      +0x17e800) whose window also holds exactly one adjacent pair, which is
//      why the rule is not general: it stays data for every other target;
//   3. KL_X18_PAIR_POOL=0/1 overrides the target either way;
//   4. two words that are NOT adjacent, or three, are data under the rule too.
//
// The switch is read once per process, so each configuration runs in a child
// with its own environment.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/kl_x18.h"

#define ROBO "x1906871599408213"

// Each window is 33 words with the site in the middle (index 16).
static const uint32_t physx[33] = {
    0x3d8002e0, 0x17ffff59, 0x42c80000, 0x42c80000, 0x00000000, 0xd503201f,
    0xd10883ff, 0x4f000407, 0xaa0203f1, 0xaa0303ec, 0x52800089, 0xaa0403ea,
    0xa9b67bfd, 0x2f000405, 0x0f07f406, 0x910003fd, 0xf9400012, 0x6d0627e8,
    0x6d072fea, 0xa90153f3, 0x6d0837ec, 0xa9025bf5, 0xbd400643, 0x6d093fee,
    0xa90363f7, 0x2f00e5e8, 0xa9046bf9, 0x0ea61ccf, 0xa90573fb, 0x0f03f60d,
    0x0e040463, 0x910683ae, 0xaa0303f5,
};
static const uint32_t p256[33] = {
    0xe9f67ee9, 0x80494264, 0x43e31740, 0x7ae1da00, 0xd9deb31c, 0xb3eb33f7,
    0x1b40f5b8, 0x309771cd, 0xac4d9cf9, 0x36d98e7e, 0x56b53992, 0x58bf4f0f,
    0xbdc3bab8, 0x6cd7b079, 0xdee24945, 0x3e9d0494, 0x9bb20a91, 0x692e5790,
    0xbf614fa4, 0xe9e3fbdb, 0x48e02681, 0x95328c90, 0x8eec388d, 0x36c35ea7,
    0x9abcd1c6, 0x2cdbbab3, 0x7450a0e4, 0x144898ef, 0xa94d38c9, 0x08d41348,
    0x5ecffd60, 0xebc7cdf2, 0x303288af,
};

enum { W_PHYSX, W_P256, W_APART, W_THREE, W_COUNT };

// The PhysX window with its pool split (the second 100.0f moved two words on)
// or grown to three words: neither is the one shape the rule admits.
static void variant(uint32_t *w, int which) {
    for (int i = 0; i < 33; i++) w[i] = physx[i];
    if (which == W_APART) { w[3] = 0x00000000; w[5] = 0x42c80000; }
    if (which == W_THREE) w[4] = 0x42c80000;
}

// One configuration's verdicts as a bit mask (bit = "data"), from a child.
static int verdicts(const char *target, const char *pair) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        if (target) setenv("KLEPTON_TARGET", target, 1); else unsetenv("KLEPTON_TARGET");
        if (pair) setenv("KL_X18_PAIR_POOL", pair, 1); else unsetenv("KL_X18_PAIR_POOL");
        int mask = 0;
        for (int k = 0; k < W_COUNT; k++) {
            uint32_t w[33];
            if (k == W_P256) for (int i = 0; i < 33; i++) w[i] = p256[i];
            else variant(w, k);
            if (kl_x18_is_data(w, sizeof w, 16)) mask |= 1 << k;
        }
        _exit(mask);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) { fprintf(stderr, "child died\n"); exit(2); }
    return WEXITSTATUS(st);
}

static int g_fail;
static void expect(const char *what, const char *target, const char *pair, int want) {
    int got = verdicts(target, pair);
    static const char *names[W_COUNT] = {"physx pool", "P-256 table", "pair apart", "three"};
    for (int k = 0; k < W_COUNT; k++) {
        int g = (got >> k) & 1, e = (want >> k) & 1;
        if (g != e) {
            g_fail = 1;
            printf("  FAIL %-34s %-12s is %s, want %s\n", what, names[k],
                   g ? "data" : "code", e ? "data" : "code");
        }
    }
    if (got == want) printf("  ok   %s\n", what);
}

int main(void) {
    printf("=== x18 two-entry literal pools ===\n");
    // Sanity: the site really is an x18 site, so the verdict is what decides it.
    klx_info in;
    klx_decode(physx[16], &in);
    if (!in.nfields) { printf("  FAIL ldr x18, [x0] did not decode as a site\n"); g_fail = 1; }

    const int all = (1 << W_PHYSX) | (1 << W_P256) | (1 << W_APART) | (1 << W_THREE);
    const int pool_on = all & ~(1 << W_PHYSX) & ~(1 << W_P256);
    expect("no target: window test unchanged", NULL, NULL, all);
    expect("Beat Saber: window test unchanged", "beatsaber", NULL, all);
    expect("Galaxy's Edge: window test unchanged", "x3484270071659289", NULL, all);
    expect("Robo Recall: lone pair is a pool", ROBO, NULL, pool_on);
    expect("Robo Recall, KL_X18_PAIR_POOL=0", ROBO, "0", all);
    expect("other target, KL_X18_PAIR_POOL=1", "beatsaber", "1", pool_on);
    printf("  (the P-256 word reads as code whenever the rule is on — the reason it is\n"
           "   scoped to the target it was verified on)\n");
    printf("%s: x18 two-entry literal pools\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
