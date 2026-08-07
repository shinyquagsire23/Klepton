// The whole C surface the Swift app sees.
//
// PLANNING §12.6: Swift owns the platform layer, C owns the guest, and the
// seam between them is a plain C header — no Objective-C anywhere. That is not
// an aesthetic choice, it is what keeps the boundary narrow: everything below
// is four functions, and the guest-facing complexity (JNIEnv, jobjects, ELF
// images) never crosses into Swift at all.
#import "kl_app.h"
// P5b widens the seam by exactly two headers, and only for the compositor:
// kl_glfb owns the eye-texture provider (the MTLTexture the guest renders
// into), and kl_ovrp owns the pose-in seam ARKit answers. Everything
// guest-facing still stays on the C side.
#import "kl_glfb.h"
#import "kl_ovrp.h"
