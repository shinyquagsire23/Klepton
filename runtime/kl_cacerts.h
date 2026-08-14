//
// The root CA anchors the synthetic Android trust store hands to the guest.
//
// Split from kl_jni.c for the reason kl_vtdec is split from kl_mediandk: this
// half is the SOURCE of the certificates and knows nothing about JNI, so it can
// be checked without a guest. kl_jni.c owns the guest-facing shape
// (TrustManagerFactory -> X509TrustManager -> X509Certificate).
//

#ifndef KL_CACERTS_H
#define KL_CACERTS_H

#include <stddef.h>

// How many anchors we will present. 0 means "no trust store", which is what
// this runtime did before the table existed and what KL_CA_ANCHORS=0 restores.
int kl_cacert_count(void);

// Anchor `i` as DER, or NULL if `i` is out of range. The bytes are static and
// immutable — the caller copies into whatever the guest is given.
const unsigned char *kl_cacert_at(int i, size_t *len);

#endif // KL_CACERTS_H
