// Half-Life: Alyx's NPC eyes flash solid red / green / blue / magenta for a frame
// or three, every few dozen frames (first seen 2026-10-06 and left open: "eyes of
// human characters flare in flat colours for 1-3 frames every 31 frames").
//
// The cause and the cure are Glovebox's (github.com/glovebox-vr/glovebox,
// klepton/runtime/gfx/kl_vulkan.c, klvk_eye_envguard, MIT, (c) 2026 the Glovebox
// contributors), whose words these are:
//
//   Every material shader in this Vulkan build unpacks the per-object value its
//   vertex shader passes on (flat, location 7) as two fields: cubemap record =
//   v >> 8 (tint / box / normalization, a 144-entry array in the scene's light
//   constant buffer) and cube-array layer = v & 255. The vr_eyeball pixel shader
//   alone is stale: it indexes the record array with the raw v (always >= 256
//   for an eye) and samples layer 0. So it reads ~97 KB into a uniform buffer
//   bound for 36-64 KB. PC GPUs bounds-check constant-buffer reads and return
//   zeros there (D3D guarantees it; NVIDIA, AMD and Adreno do it in hardware),
//   so on PC the eyes merely lose their reflection. Metal does not bounds-check
//   constant buffers: the read lands in whatever the ring buffer holds next —
//   random tints, identity-matrix rows, huge values — hence the flashes.
//
// The rewrite decodes v the way the rest of the build does: record = v >> 8 for
// the nine table reads, layer = float(v & 255) — clamped to the cube array's
// real layer count — for the two cube-array samples; and, as a safety net, the
// cubemap-reflection term is zeroed (the PC result) if a decoded record were
// still >= 144.
//
// One thing is different here. Glovebox patches ONE module, named by size and
// SHA-1 (46,924 bytes, 47da31f1…: "the combo Chapter 1 uses"). The game's
// shader file has six vr_eyeball pixel shaders that the pipeline list uses
// (23,496 to 62,184 bytes), and all six have the stale read with the same ids,
// except that one loads v under another id and two already declare the vector
// type the rewrite adds (Glovebox's function stops at that and leaves those two
// alone; here their own type is used). So the module is recognised by its
// SHAPE instead — the checks Glovebox's rewrite makes before it touches
// anything, which no other shader passes because every other shader shifts v
// before using it — and the id of the load is found rather than assumed. For
// Glovebox's module the output is byte for byte what Glovebox's own function
// writes (checked on the six modules of Steam build 25487405, 2026-10-13).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kl_vk_eyefix.h"

uint32_t *kl_vk_eye_envguard(const uint32_t *w, size_t nw, size_t *out_nw) {
    enum { IDX = 3142, TERM = 11175, COORD_A = 14604, COORD_B = 14605, ZERO_F = 2572,
           T_INT = 12, T_UINT = 11, T_FLOAT = 13, T_VEC3 = 24, T_VEC4 = 29,
           CUBES = 3424, T_CUBES = 216, INT0 = 2571, INT1 = 2574, GLSL450 = 1 };
    // Every one of the six is between 5,874 and 15,546 words with an id bound of
    // 25,242 or 25,268; anything far from that is not looked at.
    if (!w || !out_nw || nw < 4000 || nw > 24000 || w[0] != 0x07230203u || w[3] <= COORD_B) return NULL;
    // The one load of v: its id is the only one that differs between the six.
    uint32_t LOAD = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > nw) return NULL;
        if (op == 61 && wc == 4 && w[i + 3] == IDX) { if (LOAD) return NULL; LOAD = w[i + 2]; }
        i += wc;
    }
    if (!LOAD) return NULL;
    uint32_t tbool = 0; size_t firstfn = 0;
    int loads = 0, chains = 0, coords = 0, fadds = 0, ok_uint = 0, ok_vec3 = 0, other_uses = 0;
    int ok_int = 0, ok_cubes = 0, ok_consts = 0, ok_glsl = 0, cube_var = 0;
    uint32_t have_v3int = 0;    // two of the six declare a vector of three ints themselves; a type is declared once
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > nw) return NULL;
        if (op == 20) tbool = w[i + 1];                                                   // OpTypeBool
        if (op == 21 && w[i + 1] == T_UINT && w[i + 2] == 32 && w[i + 3] == 0) ok_uint = 1;
        if (op == 23 && w[i + 1] == T_VEC3 && w[i + 2] == T_FLOAT && w[i + 3] == 3) ok_vec3 = 1;
        if (op == 23 && w[i + 2] == T_INT && w[i + 3] == 3) have_v3int = w[i + 1];
        if (op == 21 && w[i + 1] == T_INT && w[i + 2] == 32 && w[i + 3] == 1) ok_int = 1;
        if (op == 25 && w[i + 1] == T_CUBES && w[i + 3] == 3 && w[i + 5] == 1) ok_cubes = 1;   // Cube, arrayed
        if (op == 59 && w[i + 2] == CUBES) cube_var = 1;
        if (op == 43 && w[i + 1] == T_INT && ((w[i + 2] == INT0 && w[i + 3] == 0) || (w[i + 2] == INT1 && w[i + 3] == 1))) ok_consts++;
        if (op == 11 && w[i + 1] == GLSL450) ok_glsl = 1;
        if (op == 54 && !firstfn) firstfn = i;                                            // first OpFunction
        if (op == 61 && wc == 4 && w[i + 3] == IDX) { loads++; if (w[i + 1] != T_UINT || w[i + 2] != LOAD) return NULL; }
        if (op == 65) { for (uint32_t k = 4; k < wc; k++) if (w[i + k] == LOAD) chains++; }   // OpAccessChain index
        else if (op != 61) { for (uint32_t k = 1; k < wc; k++) if (w[i + k] == LOAD) other_uses++; }
        if (op == 80 && wc == 7 && w[i + 1] == T_VEC4 && (w[i + 2] == COORD_A || w[i + 2] == COORD_B) && w[i + 6] == ZERO_F) coords++;
        if (op == 129 && wc == 5 && w[i + 1] == T_VEC3 && (w[i + 3] == TERM || w[i + 4] == TERM)) fadds++;
        i += wc;
    }
    if (!tbool || !firstfn || !ok_uint || !ok_vec3 || loads != 1 || chains != 9 || other_uses || coords != 2 || fadds != 1 ||
        !ok_int || !ok_cubes || !cube_var || ok_consts != 2 || !ok_glsl)
        return NULL;
    uint32_t bound = w[3];
    const uint32_t c8 = bound++, c255 = bound++, c144 = bound++, zero3 = bound++;
    const uint32_t rec = bound++, lay_u = bound++, lay = bound++;
    const uint32_t t_v3int = have_v3int ? have_v3int : bound++, img = bound++, sz = bound++, cnt = bound++, cntm1 = bound++, cntm1u = bound++, lay_c = bound++;
    uint32_t *o = malloc((nw + 64) * 4);
    if (!o) return NULL;
    size_t n = firstfn;
    memcpy(o, w, firstfn * 4);
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c8;   o[n++] = 8;               // OpConstant %uint 8
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c255; o[n++] = 255;             // OpConstant %uint 255
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c144; o[n++] = 144;             // OpConstant %uint 144
    o[n++] = (3u << 16) | 46; o[n++] = T_VEC3; o[n++] = zero3;                          // OpConstantNull %v3float
    if (!have_v3int) { o[n++] = (4u << 16) | 23; o[n++] = t_v3int; o[n++] = T_INT; o[n++] = 3; }   // OpTypeVector %int 3
    for (size_t i = firstfn; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        size_t at = n;
        if (op == 129 && wc == 5 && w[i + 1] == T_VEC3 && (w[i + 3] == TERM || w[i + 4] == TERM)) {
            uint32_t cond = bound++, sel = bound++;
            o[n++] = (5u << 16) | 176; o[n++] = tbool;  o[n++] = cond; o[n++] = rec; o[n++] = c144;          // OpULessThan
            o[n++] = (6u << 16) | 169; o[n++] = T_VEC3; o[n++] = sel; o[n++] = cond; o[n++] = TERM; o[n++] = zero3;  // OpSelect
            at = n;
            memcpy(o + n, w + i, wc * 4); n += wc;
            for (size_t k = at + 3; k < n; k++) if (o[k] == TERM) o[k] = sel;
        } else {
            memcpy(o + n, w + i, wc * 4); n += wc;
            if (op == 65) for (size_t k = at + 4; k < n; k++) if (o[k] == LOAD) o[k] = rec;     // table reads: record
            if (op == 80 && wc == 7 && (o[at + 2] == COORD_A || o[at + 2] == COORD_B)) o[at + 6] = lay;  // layer
        }
        if (op == 61 && wc == 4 && w[i + 3] == IDX) {                                   // right after the one load of v
            o[n++] = (5u << 16) | 194; o[n++] = T_UINT;  o[n++] = rec;   o[n++] = LOAD; o[n++] = c8;     // v >> 8
            o[n++] = (5u << 16) | 199; o[n++] = T_UINT;  o[n++] = lay_u; o[n++] = LOAD; o[n++] = c255;   // v & 255
            // clamped to the cube array's real layer count (Metal does not clamp an array index either)
            o[n++] = (4u << 16) | 61;  o[n++] = T_CUBES; o[n++] = img;  o[n++] = CUBES;                 // the cube array
            o[n++] = (5u << 16) | 103; o[n++] = t_v3int; o[n++] = sz;   o[n++] = img; o[n++] = INT0;     // OpImageQuerySizeLod
            o[n++] = (5u << 16) | 81;  o[n++] = T_INT;   o[n++] = cnt;  o[n++] = sz;  o[n++] = 2;        // .z = layers
            o[n++] = (5u << 16) | 130; o[n++] = T_INT;   o[n++] = cntm1; o[n++] = cnt; o[n++] = INT1;    // layers - 1
            o[n++] = (4u << 16) | 124; o[n++] = T_UINT;  o[n++] = cntm1u; o[n++] = cntm1;                // as uint
            o[n++] = (7u << 16) | 12;  o[n++] = T_UINT;  o[n++] = lay_c; o[n++] = GLSL450; o[n++] = 38;  // UMin
            o[n++] = lay_u; o[n++] = cntm1u;
            o[n++] = (4u << 16) | 112; o[n++] = T_FLOAT; o[n++] = lay;   o[n++] = lay_c;                 // float(...)
        }
        i += wc;
    }
    o[3] = bound;
    *out_nw = n;
    return o;
}
