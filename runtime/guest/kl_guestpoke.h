// Measured pokes into a LIVE guest object, as opposed to kl_guestpatch.c's
// edits to a guest image.
//
// The distinction is what decides where each one can run: an instruction is in
// the file and can be rewritten at load or offline by klepton-ld, while a field
// of an engine singleton does not exist until the guest has constructed it, so
// there is no image to edit and the store has to happen at run time. Which in
// turn is why this header exists at all — a poke that lives in one driver is a
// poke the other silently never does.
#ifndef KL_GUESTPOKE_H
#define KL_GUESTPOKE_H

// libunity's texture-unit cap, raised from Unity's un-queried 32. Call it once
// the graphics device is up and before the frame pump. It stores only when this
// guest's Unity version is one the offsets were measured against AND the raise
// is used on that build, and names the reason when it declines. It is not used
// on Beat Saber, where the engine's own 32-unit check is what keeps an
// out-of-range unit out of its GL state cache. KL_POKE_CAP=<n> sets the value
// and forces it; KL_POKE_CAP_OFF=1 leaves the guest alone.
void kl_guest_poke_texture_unit_cap(void);

#endif
