// libaaudio.so — the audio half of the Steam Link VR client.
//
// The shape is kl_opensl.c's, one API family over: everything here is the
// guest's contract, kl_audio.c is the host's, and the two meet at exactly one
// call — a feeder thread pops a buffer and hands it to kl_audio_write(), which
// blocks for as long as that audio takes to play. That blocking IS the pacing.
//
// AAudio differs from OpenSL ES in one way that matters: it is a *pull* API.
// FMOD enqueued buffers and we played them; Steam Link registers a data
// callback and we must call it. So the feeder thread runs GUEST CODE — which
// makes kl_thread_init() mandatory before the first call — and it must
// not be CoreAudio's render thread, which may not block, allocate, or take a
// lock the guest holds. Hence a thread of our own per stream, exactly as
// kl_opensl.c already does, feeding the same lock-free ring.
//
// Direction: OUTPUT is implemented, INPUT is refused by design. The guest asks
// for a capture stream for voice chat (it calls setInputPreset), and we report
// no capture device rather than fabricating one — reporting silence would be
// inventing behaviour, and a real microphone would need a privacy usage
// description whose absence is a process kill on the request. "No
// input device" is the same platform-absent answer kl_ovrplat.c gives, and it
// grants nothing. If the guest ever treats that as fatal the log says so by
// name, which is the point.
#ifndef KL_AAUDIO_H
#define KL_AAUDIO_H

#include <stdio.h>

// The ELF-import door. libvrlink_scene.so DT_NEEDEDs libaaudio.so and imports
// nineteen AAudio* symbols directly, so they have to bind at RELOCATION time,
// like tier 6's gl* and tier 7's xr*. NULL for a name we do not serve, so it
// stays in the unresolved-import report instead of becoming a silent stub.
void *kl_aaudio_lookup(const char *name);

// The DLOPEN door, which is a different door from the import one above. FMOD
// (every Unity guest) dlopen()s "libaaudio.so" and dlsym()s its way in, where
// Steam Link DT_NEEDEDs it. There is no such file in any guest tree, so without
// this the dlopen fails and FMOD reports only "failed to initialize the output
// device" — all audio gone, named nowhere near the cause.
int   kl_aaudio_claims(const char *soname);
void *kl_aaudio_dlopen(const char *soname);
int   kl_aaudio_is_handle(const void *h);
void *kl_aaudio_sym(const char *name);

void kl_aaudio_report(FILE *f);

#endif
