// kl_import — a guest asking the person for a file.
//
// On Android a game opens the system's document picker through an Intent and
// copies what comes back. Klepton runs no Java, so a guest that wants the same
// asks here instead, through two plain C calls it looks up at run time:
//
//   int (*imp)(const char *, const char *, const char *) = dlsym(RTLD_DEFAULT, "klepton_import_file");
//   int (*st)(int) = dlsym(RTLD_DEFAULT, "klepton_import_status");
//
// An Android device has neither symbol, so a guest written this way keeps its
// own picker there and uses this one only under Klepton. Optional and additive:
// nothing calls it but a guest that asks, and with no frontend registered (the
// host runner) a request is refused rather than left hanging.
//
// The frontend (the visionOS app) polls kl_import_take, shows the system file
// picker, copies the chosen file to the destination and reports the outcome.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KL_IMPORT_PENDING   = 0,   // asked, the picker not answered yet
    KL_IMPORT_COPIED    = 1,   // the file is at the destination
    KL_IMPORT_CANCELLED = 2,   // the person closed the picker
    KL_IMPORT_FAILED    = 3,   // chosen but not copied
    KL_IMPORT_UNKNOWN   = -1,  // not a ticket this run issued
};

// Guest side. `title` is what the picker window says ("Choose your GoldenEye
// ROM"), `extensions` a comma-separated hint ("z64,v64,n64", "" for any) and
// `dest` the guest path the file is copied to (replaced if there). Returns a
// ticket (> 0), or 0 when no frontend can show a picker or one is already up.
int klepton_import_file(const char *title, const char *extensions, const char *dest);
int klepton_import_status(int ticket);

// Frontend side.
void kl_import_frontend(int available);
// The request waiting to be shown: its ticket, or 0 if none. `dest` comes back
// as the HOST path to write.
int kl_import_take(char *title, size_t tcap, char *exts, size_t ecap, char *dest, size_t dcap);
void kl_import_finish(int ticket, int status);

#ifdef __cplusplus
}
#endif
