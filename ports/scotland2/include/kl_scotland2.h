#pragma once
// Opt-in native Scotland2 loader core; optional prepared scene-hook bridge.
int kl_scotland2_start(void);
void *kl_scotland2_interpose(void *handle, const char *name, void *real);
const char *kl_scotland2_error(void);
// Native exports use C only; optional C++ getters come from a real Android DSO.
void *kl_scotland2_sym(const char *name);
int kl_scotland2_claims(const char *path);
void *kl_scotland2_dlopen(const char *path);
int kl_scotland2_is_handle(void *handle);
// Scoped to the loader's module opens, including their recursive dependencies.
int kl_scotland2_strict_load(void);
void *kl_scotland2_open_module(const char *path,int flags);
// Diagnostic: follow signed dispatch and actual SDK tracker links, including
// shared targets and InstallOrigHook layers, without crossing C++ object ABIs.
#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
int kl_scotland2_verify_mod_hooks(const char *path,const char *module,unsigned expected);
#endif
