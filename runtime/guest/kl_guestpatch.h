// Measured one-instruction patches to a guest image, applied at LOAD.
//
// This is the shape `poke_texture_unit_cap` has in mains/m_boot.c generalised
// and moved where both drivers can reach it: a small table of (library, guest
// vaddr, the word that MUST be there, the word to write), applied once while
// the image is still writable. The expected word is the whole safety argument —
// it is a build fingerprint, so a patch measured against one APK is silently
// (and loudly, by name) skipped against any other rather than corrupting it.
//
// A patch is a GROUP of words under one name and is all-or-nothing: half of a
// two-instruction replacement is worse than none of it.
#ifndef KL_GUESTPATCH_H
#define KL_GUESTPATCH_H

#include <stdint.h>

// Where a guest vaddr lives in the caller's mapping. Callers differ: the
// runtime loader has one contiguous mapping and can subtract, while
// klepton-ld's buffer is indexed by FILE offset and has to walk sections. NULL
// means "not in this image", which is the ordinary answer for most rows.
typedef uint32_t *(*kl_gp_at)(void *ctx, uint64_t va);

// Apply every patch whose library basename matches `path`. Returns the number
// of patch GROUPS written. Names what it did, and names what it refused.
unsigned kl_guest_patch_apply(const char *path, kl_gp_at at, void *ctx);

#endif
