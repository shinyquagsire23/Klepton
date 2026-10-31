// Apple signed-code backend for the actual And64InlineHook public API.
// The upstream source/header and MIT license are retained in ../and64/.
#include "../and64/shared/inline-hook/And64InlineHook.hpp"
#include <android/log.h>
#include <cerrno>

extern "C" int sc2_aot_exchange(void *,void *,void **);
extern "C" int sc2_aot_error_code(void);

extern "C" __attribute__((visibility("default")))
void A64HookFunction(void *const symbol,void *const replace,void **result) {
    // Passing the actual result storage lets the native backend publish it
    // before making the replacement visible to concurrent callers.
    int status=sc2_aot_exchange(symbol,replace,result);
    if (status) errno=sc2_aot_error_code(); // ENOENT/EINVAL/ENOMEM match Android values.
    if (status) __android_log_print(ANDROID_LOG_ERROR,"A64_HOOK",
        "Prepared hook unavailable for %p (errno %d); no executable code was written",symbol,errno);
}

extern "C" __attribute__((visibility("default")))
void *A64HookFunctionV(void *const symbol,void *const replace,void *const rwx,const uintptr_t rwx_size) {
    (void)symbol; (void)replace; (void)rwx; (void)rwx_size;
    // The supplied writable-code-buffer API cannot be honored by signing a new
    // pointer after launch. Fail before changing a dispatch slot.
    errno=ENOTSUP;
    __android_log_print(ANDROID_LOG_ERROR,"A64_HOOK","Caller-supplied executable trampoline buffers are unsupported on this backend");
    return nullptr;
}
