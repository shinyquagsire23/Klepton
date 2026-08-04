// Guest-side test payload. Built twice from this one file:
//
//   * by the Android NDK into an ARM64 ELF .so, which klepton loads and
//     veneers — the allocator there uses x18 as a general-purpose temporary,
//   * by the host compiler straight into the test binary, where x18 is
//     reserved and so never allocated.
//
// Same source, two register allocations, so the two must produce identical
// numbers. That is the point: it turns an x18 bug from "one run in three
// crashes" into "this function returned the wrong value", which is a far
// stronger statement than Beat Saber not crashing 500 times in a row. A decoder
// that substitutes the wrong bit-field usually still *runs* — it just computes
// something else, and nothing but a differential check catches that.
//
// tests/t_guest.c drives it and refuses to pass if the built .so contains no
// x18 instructions at all, which would make the whole comparison vacuous.
#include <stdint.h>
#include "torture.h"

// Deliberately register-hungry: sixteen values live across the loop body, all
// feeding each other, so the allocator runs out of callee-saved registers and
// reaches for x18. The arithmetic is arbitrary — only determinism matters. It
// mixes widths and shapes on purpose so the veneered instruction set is not all
// one encoding: 64- and 32-bit forms, multiplies (madd), bitfield inserts,
// rotates, byte loads and conditional branches.
uint64_t kt_mix(uint64_t seed, int rounds) {
    uint64_t a = seed ^ 0x9E3779B97F4A7C15ULL, b = seed + 0x123456789ABCDEFULL;
    uint64_t c = seed * 0x2545F4914F6CDD1DULL, d = ~seed;
    uint64_t e = seed << 13, f = seed >> 7, g = seed ^ 0xA5A5A5A5A5A5A5A5ULL;
    uint64_t h = seed + 0x0F0F0F0F0F0F0F0FULL;
    uint32_t p = (uint32_t)seed, q = (uint32_t)(seed >> 32);
    uint32_t r = p ^ 0xDEADBEEFu, s = q + 0xCAFEBABEu;
    uint8_t  tbl[64];

    for (int i = 0; i < 64; i++) tbl[i] = (uint8_t)(i * 7 + (seed & 0xff));

    for (int i = 0; i < rounds; i++) {
        a += b ^ (c >> 3);
        b ^= (d << 5) | (d >> 59);            // rotate: extr
        c += (uint64_t)p * (uint64_t)q + h;   // madd
        d ^= e + (f << 1);
        e = (e << 7) ^ (g >> 11);
        f += tbl[(a >> 3) & 63];              // ldrb, and an index the allocator
        g ^= tbl[(b >> 5) & 63];              // has to keep live
        h += (uint64_t)r * 3 + s;

        p = (p << 3) ^ (uint32_t)(a >> 32);   // 32-bit forms: half the real
        q += (uint32_t)(b & 0xffff) | 1u;     // corpus is w18, not x18
        r ^= (uint32_t)(c >> 17);
        s = (s >> 2) + (uint32_t)(d & 0xff);

        if ((i & 15) == 0) {                  // cbz/tbz shapes
            a ^= h;
            b += g;
        }
        if (a & 1) c = (c << 1) | 1;
        else       c = c >> 1;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^
           ((uint64_t)p << 32) ^ q ^ ((uint64_t)r << 16) ^ s;
}

// A second shape: recursion plus a struct returned by value, so the veneer is
// exercised across call boundaries and stack traffic rather than only inside a
// hot loop.
kt_pair kt_walk(uint64_t v, int depth) {
    if (depth <= 0) {
        kt_pair p = {v ^ 0xFEEDFACECAFEBEEFULL, v + 1};
        return p;
    }
    kt_pair l = kt_walk(v * 3 + 1, depth - 1);
    kt_pair r = kt_walk(v ^ (v >> 5), depth - 2 > 0 ? depth - 2 : 0);
    kt_pair o = {l.lo ^ r.hi ^ kt_mix(v, 8), l.hi + r.lo + depth};
    return o;
}

// The direct probe, and the guest-side counterpart of spikes/s05_x18.c: leave a
// value in x18, burn time in userspace without issuing a syscall, read it back.
// On bare Darwin this loses the value to any timer interrupt. Under veneering
// the write and the read both go to the per-thread shadow instead, so nothing
// the kernel does to the architectural register can be observed. Returns the
// number of losses, which must be zero.
//
// Host-side this cannot even be compiled — clang rejects x18 in a clobber list
// on Darwin — which is itself the cleanest statement of the whole problem.
#if !defined(__APPLE__)
uint64_t kt_x18_direct(int iters, uint64_t magic) {
    uint64_t lost = 0;
    for (int i = 0; i < iters; i++) {
        uint64_t out;
        __asm__ volatile("mov x18, %0" :: "r"(magic) : "x18");
        for (volatile int k = 0; k < 200; k++) { }   // no calls: only preemption
        __asm__ volatile("mov %0, x18" : "=r"(out));
        if (out != magic) lost++;
    }
    return lost;
}
#endif
