// The frontend seam for a FLAT guest — the two halves a window needs.
//
// kl_present.h says a guest is producing one flat image and therefore wants a
// window; this is what a window then has to talk to. Both halves already
// existed inside kl_view.c, which is SDL3 and host-only, so neither was
// available to the visionOS app — and the app is a window frontend for exactly
// the same guest. Everything here is SDL-free and in RUNTIME_SHIP; kl_view.c
// calls it too, so there is one implementation of each rather than a host one
// and a device one that will disagree.
//
//   IN    the pointer and the keyboard, through the guest's OWN Android input
//         path — SDLActivity.onNativeMouse / onNativeKeyDown / onNativeKeyUp,
//         which SDL3 registered with us at JNI_OnLoad. Nothing is invented: we
//         author the Java side, so events arrive exactly as SDLSurface.onTouch
//         would have sent them and SDL's own Android backend does the
//         translating.
//   OUT   the newest frame the guest presented, as the readback buffer
//         kl_glfb's capture already produces. A flat guest's picture is the
//         default framebuffer of an EGL window surface — there are no eye
//         textures and no stages, so the zero-copy MTLTexture route the stereo
//         path uses does not apply to it.
//
// Costs nothing for a guest that is not SDL: the native lookups return NULL for
// Unity, which registers no such natives, and every call becomes a no-op.
#ifndef KL_MONO_H
#define KL_MONO_H

#include <stdint.h>

// --- input ------------------------------------------------------------------
//
// android.view.MotionEvent's constants, spelled rather than guessed — SDL3's
// Android_OnMouse switches on the action and derives WHICH button changed by
// diffing the button-state mask against the last one, so the state passed to
// kl_mono_pointer must be the state AFTER the transition, not the button that
// caused it.
#define KL_MONO_DOWN        0
#define KL_MONO_UP          1
#define KL_MONO_MOVE        2
#define KL_MONO_HOVER_MOVE  7
#define KL_MONO_SCROLL      8
#define KL_MONO_BTN_PRIMARY    1
#define KL_MONO_BTN_SECONDARY  2
#define KL_MONO_BTN_TERTIARY   4

// Whether the guest registered the natives at all. Resolved lazily on first
// use, because RegisterNatives runs long after any frontend's first line; a
// frontend can call this to say "display only" out loud instead of silently
// dropping every event.
int kl_mono_input_available(void);

// `x`/`y` are in the guest's SURFACE pixels, not the frontend's window points.
// The frontend owns that scaling, because only it knows what its own drawable
// is — and a pointer that lands in the wrong place is the kind of bug that
// reads as "the UI ignores clicks".
void kl_mono_pointer(int state, int action, float x, float y);

// Android keycodes, and 0 means "no key I am prepared to name" — dropped
// rather than guessed at, because a wrong keycode is a keypress the guest acts
// on, which is worse than none.
void kl_mono_key(int down, int keycode);

// KL_VIEW_POKE="fx,fy@secs[;fx,fy@secs]..." — a SEQUENCE of synthetic clicks at
// FRACTIONAL surface coordinates, each `secs` after the guest presented its
// first frame. Call once per frontend frame; it is a no-op with the knob unset,
// and it stops on its own when the sequence is spent.
//
// It exists because the alternative for proving the input path is posting a
// real event at the desktop, which clicks whatever window is actually under
// that point — it found an editor the first time it was tried, and inside the
// visionOS simulator there is no window to click at all. This drives the same
// three calls a real click drives, so it proves the path and not the plumbing.
//
// A sequence rather than a single click because the interesting screens are not
// the first one: the shell's host list is two clicks in, and pairing is three.
// Every `secs` is measured from the SAME zero, not from the previous click, so
// a run is described by when each screen is expected rather than by durations
// that have to be re-derived when an earlier one gets slower. A poke whose
// deadline has already passed fires as soon as the one before it finishes, so
// the order is always the written one.
//
// Frontend-agnostic on purpose: the fractions are of the guest's own surface,
// which is what both the SDL viewer and the visionOS window ultimately scale
// into, so one implementation serves both and a click means the same thing on
// each.
void kl_mono_poke_tick(void);

// A printable character (or one of the few control characters a text field
// produces) as the Android keycode for it. The mapping is here rather than in
// each frontend for the reason above: there is one list of keys we are willing
// to claim, and it should not fork per platform. Returns 0 for anything absent.
int  kl_mono_keycode_for_char(int ch);

// --- frame out --------------------------------------------------------------

// Register the kl_glfb frame sink. Must be called BEFORE the guest reaches its
// first swap: kl_glfb only captures at all when a sink or a dump directory is
// set, so a late registration is a window that stays black with nothing saying
// why. Safe to call more than once.
void kl_mono_capture_start(void);

// Borrow the newest presented frame. Returns 0 and touches nothing when the
// guest has not presented one yet.
//
// **Rows are BOTTOM-UP RGBA**, exactly as glReadPixels produced them — the flip
// is the consumer's, because a GPU consumer gets it free in a texture
// coordinate and a CPU one does not.
//
// `serial` counts frames since the process started, so a consumer can tell "no
// new frame" from "a new frame that happens to look the same". Unlock as soon
// as the copy is done: the sink runs inside the guest's frame and blocks on
// this lock.
int  kl_mono_frame_lock(const uint8_t **rgba, int *w, int *h, uint32_t *serial);
void kl_mono_frame_unlock(void);

// How many frames the sink has taken. Readable without locking, for a consumer
// that only wants to know whether anything is arriving.
uint32_t kl_mono_frame_count(void);

#endif
