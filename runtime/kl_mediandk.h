// Synthetic handles for libmediandk.so (AMediaCodec/AMediaFormat) and
// libOpenMAXAL.so. Neither is implemented — every symbol resolves to a stub
// that aborts naming itself, so a guest that reaches video decode dies at the
// exact entry point rather than at a null call. The Steam Link menu does not
// touch either library; the real AMediaCodec -> VideoToolbox mapping is the
// target's main event (PLANNING §11.4) and lands separately.
#ifndef KL_MEDIANDK_H
#define KL_MEDIANDK_H

void *kl_mediandk_dlopen(const char *soname);   // NULL if not one of ours
int   kl_mediandk_is_handle(const void *h);
void *kl_mediandk_sym(const char *name);        // never NULL

#endif
