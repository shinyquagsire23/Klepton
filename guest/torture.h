// Shared between the guest build (Android NDK, ARM64 ELF) and the host build
// (Darwin arm64, linked straight into tests/t_guest). Both come from
// guest/torture.c — see the comment there for why the same source is compiled
// twice.
#ifndef KT_TORTURE_H
#define KT_TORTURE_H
#include <stdint.h>

typedef struct { uint64_t lo, hi; } kt_pair;

uint64_t kt_mix(uint64_t seed, int rounds);
kt_pair  kt_walk(uint64_t v, int depth);

// Guest-only: clang will not compile an x18 clobber on Darwin at all.
uint64_t kt_x18_direct(int iters, uint64_t magic);

#endif
