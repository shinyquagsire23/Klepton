// SL-8 front door — libklepton_openxr, the replacement for the Khronos loader.
//
// PLANNING §11.8 settled the call and it is §3.1's argument verbatim, one API
// later: `libopenxr_loader.so` is the Khronos *loader*, and a loader's whole job
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
// One measured correction to §11.8, which claimed libvrlink_scene imports "six
// xr* symbols by name" and reaches the rest through xrGetInstanceProcAddr.
// It imports **forty-six by name** (t_load and the .dynsym agree), and
// xrGetInstanceProcAddr is merely one of them. That is good news twice over:
// the whole surface is enumerable statically instead of arriving one dlsym at a
// time, and every name binds at RELOCATION time, so an entry point we do not
// serve is a named unresolved import rather than a null pointer.
//
// The rule from M6/M7 carries over unchanged, and it is the reason this file
// started out mostly refusals: **lookups are measurements, calls are
// assertions.** xrGetInstanceProcAddr answers for anything we have, because the
// guest resolves plenty it never calls. Calling something we have not built
// aborts BY NAME, so the trace says what the runtime actually needs rather than
// us guessing at fifty functions the app may never touch.
//
// SL-9 followed that trace and built what it named: the instance/system boot,
// the session and its event queue, spaces, swapchains, the actions surface, and
// the frame loop. Steam Link's VR half now runs on it. What is still refused is
// what it has never called.
//
// Two entry points are NOT in the import list and arrive only through
// xrGetInstanceProcAddr — xrInitializeLoaderKHR and
// xrGetOpenGLESGraphicsRequirementsKHR. Both were found by the "not served"
// line in that function, and both had to be *served* rather than merely
// reported: this guest checks the result, logs the failure, and then calls the
// null pointer anyway. A name in that line is a SIGSEGV at 0x0 a moment later.
#ifndef KL_OPENXR_H
#define KL_OPENXR_H
#include <stdio.h>

// Resolve an xr* import. NULL if the name is not one we know, so the
// unresolved-import report still works — see the note in kl_shim_lookup about
// a gateway that can never say no.
void *kl_openxr_lookup(const char *name);

// Which xr* the guest resolved and which it called — the SL-8 work list, in the
// shape kl_ovrp_report prints for M6.
void kl_openxr_report(FILE *f);

#endif
