// The OpenXR front door — libklepton_openxr, replacing the Khronos loader.
//
// The same argument that replaces OVRPlugin, one API later:
// `libopenxr_loader.so` is the Khronos *loader*, and a loader's whole job
// is to find a runtime. On Android it does that through an
// `org.khronos.openxr.runtime_broker` service, which this environment does not
// have and cannot be given — the APK's own `<queries>` name the broker, so the
// dependency is declared, not inferred. So the loader is REPLACED, not
// translated, exactly as libOVRPlugin.so was, and for the same reason.
//
// It is a much better version of that job than OVRPlugin was: OpenXR is a
// published specification with fixed structure layouts, where OVRPlugin's 466
// entry points had to be reverse-engineered one abort at a time.
//
// libvrlink_scene does not import "six xr* symbols by name" and reach the rest
// through xrGetInstanceProcAddr.
// It imports FORTY-SIX by name (t_load and the .dynsym agree), and
// xrGetInstanceProcAddr is merely one of them. That is good news twice over:
// the whole surface is enumerable statically instead of arriving one dlsym at a
// time, and every name binds at RELOCATION time, so an entry point we do not
// serve is a named unresolved import rather than a null pointer.
//
// The rule that shapes this file: lookups are measurements, calls are
// assertions. xrGetInstanceProcAddr answers for anything we have, because the
// guest resolves plenty it never calls. Calling something we have not built
// aborts BY NAME, so the trace says what the runtime actually needs rather than
// us guessing at fifty functions the app may never touch.
//
// What the trace named is built: the instance/system boot, the session and its
// event queue, spaces, swapchains, the actions surface, and the frame loop.
// Steam Link's VR half runs on it. What is refused is what it has never
// called.
//
// Two entry points are NOT in the import list and arrive only through
// xrGetInstanceProcAddr — xrInitializeLoaderKHR and
// xrGetOpenGLESGraphicsRequirementsKHR. Both must be SERVED rather than merely
// reported: this guest checks the result, logs the failure, and then calls the
// null pointer anyway, so a name in the "not served" line is a SIGSEGV at 0x0 a
// moment later.
#ifndef KL_OPENXR_H
#define KL_OPENXR_H
#include <stdio.h>

// Resolve an xr* import. NULL if the name is not one we know, so the
// unresolved-import report still works — see the note in kl_shim_lookup about
// a gateway that can never say no.
void *kl_openxr_lookup(const char *name);

// The DLOPEN door, for a guest that opens the loader rather than importing from
// it. libUnityOpenXR.so does exactly that, and unlike the other synthetic
// libraries here the real libopenxr_loader.so IS present in a Unity guest tree
// — so without this the Khronos loader loads and goes looking for the Android
// runtime broker it needs. See the long note in kl_openxr.c.
int   kl_openxr_claims(const char *soname);
void *kl_openxr_dlopen(const char *soname);
int   kl_openxr_is_handle(const void *h);
void *kl_openxr_sym(const char *name);

// Which xr* the guest resolved and which it called — the work list, in the
// shape kl_ovrp_report prints.
void kl_openxr_report(FILE *f);

// The reference-space algebra, checked with no session, no guest and no Steam
// host. Returns non-zero if every invariant holds; `make xrspace`.
//
// It is a gate rather than a run because the failure it catches is a silently
// wrong POSE: every call succeeded, the picture was correct, and the only
// instrument that could see it was a person turning their head — which on this
// arc costs a fresh pairing and cannot be repeated identically.
int kl_openxr_space_selftest(FILE *f);

// The action surface, checked the same way and for the same reason.
//
// The input path's failures are all SILENT: a binding decoded to the wrong bit,
// a hand combined the wrong way, an inactive action reporting a stale press, an
// action space following the other hand. Every one of those returns XR_SUCCESS,
// draws a correct picture, and is visible only to a person holding a controller
// in a live stream — which on this arc costs a fresh pairing and cannot be
// repeated identically. So it is asserted here instead: no guest, no headset,
// no Steam host. `make xrinput`.
//
// It drives the REAL entry points through xrGetInstanceProcAddr, so what it
// checks is the surface the guest calls rather than a copy of it.
int kl_openxr_input_selftest(FILE *f);

// The frame clock, for a host that has one.
//
// xrWaitFrame is where an OpenXR runtime is specified to block until the app
// should begin its next frame — it is the runtime's one chance to say "not
// yet". On the command line nothing is presenting, so it returns immediately
// and the guest free-runs, which is right: there is no display to be late for.
// Inside the visionOS app there IS one, and Compositor Services owns its
// deadline, so this is where the two meet — the callback blocks until the
// compositor has published another pose.
//
// It is installed rather than compiled in because "who is the clock" is the
// driver's knowledge, exactly as the front-door handoff is (kl_jni.h). NULL is
// the default and must stay so: `make slink-vr-run` has no compositor and a
// pacer there would block forever, which is a hang with no error surface.
//
// The callback must return on its own if no pose arrives — a stalled display
// should make the guest render against the last pose, not wedge it.
void kl_openxr_set_frame_pacer(void (*wait)(void));

// Capture the TOPMOST projection layer instead of the base one (layer 0).
//
// The compositor shows exactly one projection layer (kl_glfb draws one quad per
// eye out of one array texture — see xrEndFrame). For a guest that submits a
// single projection layer that is layer 0 and there is nothing to choose. Steam
// Link submits several: layer 0 is the environment/room (measured black in an
// empty space), and the streamed screen is composited ABOVE it. OpenXR draws
// projection layers back-to-front in submission order, so the last one is the
// one on top — the picture — and the base one is the backdrop behind it. This
// makes the capture follow that rule for a guest that stacks layers, without
// the runtime having to read every layer back to find the lit one.
//
// It is a driver's fact ("this guest stacks projection layers"), not the
// runtime's, so the driver states it — exactly as the front-door handoff and
// the frame pacer are installed rather than compiled in. KL_XR_CAPTURE_LAYER,
// when set, still pins one index and overrides this.
void kl_openxr_set_capture_topmost_layer(int on);

#endif
