#pragma once
#include <stddef.h>
// One optional built-in guest extension, registered during native startup.
// Callbacks and their code/data have process lifetime. Registration completes
// before threads load guests; replacement/unregistration are not supported.
typedef struct {
    unsigned version;
    size_t size;
    int (*start)(void);
    const char *(*error)(void);
    void *(*symbol)(const char *);
    int (*claims)(const char *);
    void *(*open)(const char *);
    int (*is_handle)(void *);
    void *(*interpose)(void *,const char *,void *);
    int (*strict_load)(void);
} kl_guest_extension;
int kl_extension_register(const kl_guest_extension *api);
int kl_extension_start(void);
const char *kl_extension_error(void);
void *kl_extension_symbol(const char *name);
int kl_extension_claims(const char *path);
void *kl_extension_open(const char *path);
int kl_extension_is_handle(void *handle);
void *kl_extension_interpose(void *handle,const char *name,void *real);
int kl_extension_strict_load(void);
// The extension is linked in AND this run asked for it (KL_SCOTLAND2=1).
// Additions made for the mod port only take effect while this holds.
int kl_extension_active(void);
