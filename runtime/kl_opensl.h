// OpenSL ES — the audio output device Unity's FMOD opens.
//
// Like GLES, this arrives by dlopen + dlsym rather than as ELF imports: FMOD is
// linked into libunity.so and resolves `slCreateEngine` and the SL_IID_* symbols
// at runtime. So there is nothing in the unresolved-import list to find it by,
// and the only evidence it exists is a failed dlopen followed by the guest
// aborting itself with "sem_wait failed".
#ifndef KL_OPENSL_H
#define KL_OPENSL_H
#include <stdio.h>

void *kl_opensl_dlopen(const char *soname);   // NULL if not libOpenSLES.so
int   kl_opensl_is_handle(const void *h);
void *kl_opensl_sym(const char *name);

// What FMOD looked up and what it called. The work list, same as kl_egl_report.
void kl_opensl_report(FILE *f);

#endif
