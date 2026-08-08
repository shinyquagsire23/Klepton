// What SHAPE of picture is the guest producing — and therefore what kind of
// surface does it need presenting on?
//
// Two targets, two answers, and they are not variations of one renderer:
//
//   Beat Saber  renders a PAIR of eye images per frame, against a head pose and
//               a frustum we handed it. It needs a stereo, head-locked surface:
//               an immersive space on visionOS, the compositor's drawable.
//   Steam Link  renders ONE flat image into an EGL window surface, with no pose
//               and no stereo anywhere in it (PLANNING §11.7). It needs a
//               window.
//
// **This file does not decide which; it observes which.** The mode is derived
// from what the guest actually did — a window surface was created, or an eye
// texture was set up — rather than from a knob someone has to remember to set
// per target. A flag would be a second source of the same truth, and the two
// would drift the first time a guest did something unexpected. (The same
// reasoning as the display "group answer" in kl_jni.c: one fact, one place.)
//
// **Why a generation counter and not just a getter.** On macOS a mode change is
// a different shader, and polling a getter each frame would be enough. On
// visionOS it is not: a 2D guest wants a plain SwiftUI window and a VR guest
// wants an immersive space, and moving between those means openImmersiveSpace /
// dismissImmersiveSpace — asynchronous scene lifecycle, on the main actor, not
// something a render loop can do inline. So a consumer needs to detect that a
// transition HAPPENED and act on it once, which is what the generation is for.
// Steam Link's own 2D->VR handoff (§11.9: the 2D activity starts the OpenXR
// NativeActivity and finishes itself) is exactly this transition, so it is not
// hypothetical for this target.
//
// **Presentation is the consumer's business.** This says "mono" or "stereo"; it
// does not say "quad at 500 m" or "SwiftUI window". Placing a mono image on a
// quad inside an immersive space via kl_reproject would work — the pass already
// takes a NULL render pose and produces an unreprojected picture — but a real
// window is the better answer on visionOS, and this seam deliberately does not
// prejudge that.
#ifndef KL_PRESENT_H
#define KL_PRESENT_H

typedef enum {
    KL_PRESENT_NONE = 0,   // the guest has not produced anything yet
    KL_PRESENT_MONO,       // one flat image, no pose  -> a window
    KL_PRESENT_STEREO,     // an eye pair with a pose  -> an immersive surface
} kl_present_mode;

// What the guest is producing now.
kl_present_mode kl_present_mode_now(void);

// Bumped every time the mode CHANGES. A consumer that must do work on a
// transition (open or dismiss an immersive space) records this and compares,
// rather than trying to spot the change in a value it polls. Starts at 0.
unsigned kl_present_generation(void);

// The mono image's size, in pixels — the guest's window surface. Zero until a
// window surface exists. Meaningless in stereo mode, where the size is per eye
// and belongs to kl_glfb's eye-texture record.
void kl_present_mono_size(int *w, int *h);

// ---- producer side: called by the runtime, not by frontends ----------------

// kl_egl's eglCreateWindowSurface. A guest that asks for a window surface is
// telling us it has a flat picture to put in it.
void kl_present_note_window_surface(int w, int h);

// kl_glfb, when an eye texture is bound. Stereo WINS over mono and is sticky
// for as long as eye textures exist: Unity creates a window surface too (it is
// an Android app), and reading that as "2D" would put Beat Saber in a window.
// The eye pair is the more specific fact, so it is the one that decides.
void kl_present_note_eye_texture(void);

// Drop back to whatever the surfaces now say — for a guest that tears its eye
// textures down. Separate from the note above so the sticky rule stays
// explicit rather than being an accident of call order.
void kl_present_clear_eye_textures(void);

#endif
