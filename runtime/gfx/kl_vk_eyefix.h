// The rewrite of Half-Life: Alyx's vr_eyeball pixel shaders that ends the colour
// flashes in NPC eyes (kl_vk_eyefix.c has the why).
#pragma once
#include <stddef.h>
#include <stdint.h>

// A SPIR-V module of `nw` words. Returns a malloc'd rewritten copy (its length in
// *out_nw) when the module is one of those shaders and has every id the rewrite
// relies on in the expected shape; NULL — and nothing to free — for any other
// module.
uint32_t *kl_vk_eye_envguard(const uint32_t *w, size_t nw, size_t *out_nw);
