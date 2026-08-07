// See kl_mediandk.h. One gateway for both libraries; the distinction matters
// only in the log line.
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "kl_mediandk.h"

void kl_unresolved_named(const char *name);     // kl_shim.c

static const char g_md_handle[]  = "klepton-mediandk";
static const char g_oma_handle[] = "klepton-openmaxal";

void *kl_mediandk_dlopen(const char *soname) {
    if (!soname) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    if (strcmp(b, "libmediandk.so") == 0) {
        fprintf(stderr, "  [mediandk] guest dlopen(\"%s\") -> stub handle "
                "(decode is not implemented)\n", b);
        return (void *)g_md_handle;
    }
    if (strcmp(b, "libOpenMAXAL.so") == 0) {
        fprintf(stderr, "  [mediandk] guest dlopen(\"%s\") -> stub handle "
                "(OpenMAX AL is not implemented)\n", b);
        return (void *)g_oma_handle;
    }
    return NULL;
}

int kl_mediandk_is_handle(const void *h) {
    return h == (const void *)g_md_handle || h == (const void *)g_oma_handle;
}

void *kl_mediandk_sym(const char *name) {
    return kl_named_stub(name, (void *)kl_unresolved_named);
}
