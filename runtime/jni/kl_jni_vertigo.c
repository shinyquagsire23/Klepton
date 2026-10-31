// Arizona Sunshine 2 (x5245041552210029) reads its own logcat through Vertigo's
// Android helper right after its language is set, and the run stopped on the
// name. LogcatUtility.readLogs(args) runs `logcat -d <args>` and hands back
// what it printed. There is no logcat on this host, so the answer is what an
// empty log prints: an empty string — not NULL, which managed code would have
// to survive. Nothing of the host's own log is handed over. For that one
// title, and for this one call.
#include <string.h>
#include "guest/kl_driver.h"
#include "kl_jni.h"
#include "kl_jni_int.h"

static klj_val read_logs_empty(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("")};
}

const klj_binding *klj_vertigo_binding(const char *cls, const char *name, const char *sig) {
    static const klj_binding binding = {
        "com/vertigogames/vertigoandroidutils/LogcatUtility", "readLogs",
        "(Ljava/lang/String;)Ljava/lang/String;", read_logs_empty
    };
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x5245041552210029") ||
        strcmp(cls, binding.cls) || strcmp(name, binding.name) ||
        strcmp(sig, binding.sig)) return NULL;
    return &binding;
}
