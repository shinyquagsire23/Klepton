// The whole C surface the Swift app sees.
//
// Swift owns the platform layer, C owns the guest, and the
// seam between them is a plain C header — no Objective-C anywhere. That is not
// an aesthetic choice, it is what keeps the boundary narrow: everything below
// is four functions, and the guest-facing complexity (JNIEnv, jobjects, ELF
// images) never crosses into Swift at all.
#import "kl_app.h"
// The compositor widens the seam by three headers, and only for the compositor: kl_glfb
// owns the eye-texture provider (the MTLTexture the guest renders into),
// kl_ovrp owns the pose-in seam ARKit answers and the stage-keyed record of
// what each frame was rendered with, and kl_reproject owns the composite pass
// that consumes that record — shader and matrices both, shared with the macOS
// viewer so the picture is only ever debugged once. Everything guest-facing
// still stays on the C side.
#import "kl_glfb.h"
#import "kl_ovrp.h"
#import "kl_openxr.h"
#import "kl_reproject.h"
// ...and by one more for audio: kl_audio owns the CoreAudio output unit, but
// the AVAudioSession it needs on this platform can only be configured from
// Swift (KleptonAudio.swift). Three calls cross: the session's measured rate
// going in, and the two ways the OS takes the stream away coming back.
#import "kl_audio.h"
// ...and by two for the flat guest's window. kl_present says WHAT SHAPE
// of picture the guest is producing — one flat image wants a WindowGroup, an
// eye pair wants the ImmersiveSpace — and kl_mono is what a window then talks
// to: the newest presented frame out, the pointer and the keyboard in. Both are
// shared with kl_view.c, so the host viewer and this app cannot disagree about
// what a click is.
#import "kl_present.h"
#import "kl_mono.h"
// ...and by one for the VULKAN guest's frame seam. The compositor needs one
// thing from it and nothing else: WHEN a Vulkan guest's eye picture is
// complete. On the GL path that is an MTLSharedEvent kl_glfb signals from
// inside ANGLE's command stream; there is no such event on this path, so
// kl_vulkan publishes a serial instead and the compositor reads it exactly
// where it reads the fence value. The eye TEXTURE still arrives through
// kl_glfb's own table, so nothing else about this seam widens.
#import "kl_vulkan.h"
// ...and by one for the panel that shows how the game runs (KL_STATS_CARD):
// the compositor counts what it presents into it and reads back a sample a
// second.
#import "kl_metrics.h"
// ...and by one for the play area (KleptonRoom*.swift): the room scan's grid,
// the suggested and drawn areas, the distance to their edge the virtual wall is
// drawn from, and finding a room measured before.
#import "kl_room.h"
// One call, declared here rather than importing kl_mediandk.h: that header is
// the guest-facing media table and wants <stdio.h> and CoreVideo context the
// Swift side has no other use for.
void kl_mediandk_set_video_default(int on);
// ...and two from the Oculus Platform stand-in (kl_ovrplat.c), for the start
// window's "Player name": is this a game that is handed the offline user, and
// is it one whose user gets an id of this installation's own.
int kl_ovrplat_offline_user_for(const char *target);
int kl_ovrplat_user_id_per_install(const char *target);
// ...and Surreal Interactive's controller API (KleptonSurrealSDK.swift), which
// is linked only into a Surreal build that has their framework; see kl_surreal_sdk.h.
#import "kl_surreal_sdk.h"

#import "kl_mprobe.h"
