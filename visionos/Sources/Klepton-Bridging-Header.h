// The whole C surface the Swift app sees.
//
// PLANNING §12.6: Swift owns the platform layer, C owns the guest, and the
// seam between them is a plain C header — no Objective-C anywhere. That is not
// an aesthetic choice, it is what keeps the boundary narrow: everything below
// is four functions, and the guest-facing complexity (JNIEnv, jobjects, ELF
// images) never crosses into Swift at all.
#import "kl_app.h"
// P5b widens the seam by three headers, and only for the compositor: kl_glfb
// owns the eye-texture provider (the MTLTexture the guest renders into),
// kl_ovrp owns the pose-in seam ARKit answers and the stage-keyed record of
// what each frame was rendered with, and kl_reproject owns the composite pass
// that consumes that record — shader and matrices both, shared with the macOS
// viewer so the picture is only ever debugged once. Everything guest-facing
// still stays on the C side.
#import "kl_glfb.h"
#import "kl_ovrp.h"
#import "kl_reproject.h"
// ...and by one more for audio: kl_audio owns the CoreAudio output unit, but
// the AVAudioSession it needs on this platform can only be configured from
// Swift (KleptonAudio.swift). Three calls cross: the session's measured rate
// going in, and the two ways the OS takes the stream away coming back.
#import "kl_audio.h"
