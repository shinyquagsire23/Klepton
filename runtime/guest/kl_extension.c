#include "kl_extension.h"
#include <errno.h>
#include <pthread.h>
#include "kl_env.h"
#ifdef KL_GUEST_EXTENSION_BUILTIN
extern void kl_guest_extension_builtin(void);
static pthread_once_t builtin_once = PTHREAD_ONCE_INIT;
static void ensure_builtin(void) { pthread_once(&builtin_once,kl_guest_extension_builtin); }
#else
static void ensure_builtin(void) {}
#endif
static const kl_guest_extension *extension;
int kl_extension_register(const kl_guest_extension *api) {
    if (extension || !api || api->version!=1 || api->size!=sizeof *api ||
        !api->start || !api->error || !api->symbol || !api->claims || !api->open ||
        !api->is_handle || !api->interpose || !api->strict_load) { errno=EINVAL; return -1; }
    extension=api;
    return 0;
}
int kl_extension_start(void) { ensure_builtin(); return extension?extension->start():0; }
const char *kl_extension_error(void) { ensure_builtin(); return extension?extension->error():""; }
void *kl_extension_symbol(const char *name) { ensure_builtin(); return extension?extension->symbol(name):NULL; }
int kl_extension_claims(const char *path) { ensure_builtin(); return extension?extension->claims(path):0; }
void *kl_extension_open(const char *path) { ensure_builtin(); return extension?extension->open(path):NULL; }
int kl_extension_is_handle(void *handle) { ensure_builtin(); return extension?extension->is_handle(handle):0; }
void *kl_extension_interpose(void *handle,const char *name,void *real) { ensure_builtin(); return extension?extension->interpose(handle,name,real):NULL; }
int kl_extension_strict_load(void) { ensure_builtin(); return extension?extension->strict_load():0; }
int kl_extension_active(void) {
    static int on = -1;
    if (on < 0) { ensure_builtin(); on = extension && kl_env_on("KL_SCOTLAND2", 0); }
    return on;
}
