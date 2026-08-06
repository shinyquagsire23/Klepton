// M6 front door — libklepton_ovrp, the replacement for Oculus's OVRPlugin.
//
// PLANNING §3.1 settled this before any code existed: libOVRPlugin.so links
// against libossdk.so / libossdk.oculus.so / libOVRMrcLib.so and NEEDs
// libvrapi.so, none of which ship in the APK. It cannot be translated, so the XR
// cut line is *above* VrApi and OVRPlugin is replaced wholesale. That also
// terminates the chain: libvrapi.so is never loaded at all.
//
// The trace confirmed it the hard way. Unity resolved
// ClassLoader.findLibrary("OVRPlugin") to the real library, we loaded it,
// UnityPluginLoad ran, and it died at libOVRPlugin.so+0x2ca20 on `ldr x8, [x0]`
// with x0 NULL — a vtable dispatch through state that only exists once VrApi has
// initialised it.
//
// So we serve the soname ourselves. Same shape as the GL and OpenSL gateways:
// a synthetic handle, real implementations where we have them, and a named
// trampoline for everything else so the guest says which of the 466 ovrp_*
// entry points it actually wants rather than dying anonymously.
#ifndef KL_OVRP_H
#define KL_OVRP_H
#include <stdio.h>

void *kl_ovrp_dlopen(const char *soname);   // NULL if this is not OVRPlugin
int   kl_ovrp_is_handle(const void *h);
void *kl_ovrp_sym(const char *name);

// Which ovrp_* the guest resolved and which it called — the M6 work list.
void kl_ovrp_report(FILE *f);

// The frontend seam, pose in: whoever owns the window tells us where the head
// is, and ovrp_GetNodePoseState reports it back to the guest. Today that is the
// SDL viewer (KL_VIEW=1, kl_view.c) driven by WASD + mouse-look; on visionOS it
// is ARKit's WorldTrackingProvider answering the same call. Without a frontend
// the pose stays identity, which is what every headless run has always seen.
void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw);

#endif
