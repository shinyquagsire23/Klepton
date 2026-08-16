// java.lang: Class, ClassLoader, StringBuilder
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include "kl_jni_int.h"

// ------------------------------------------------------------ Java class impls
// Host implementations of the Java methods the guest actually calls. This stays
// here while it is small; it splits into its own file once it fills out.

// Unity asks the Context for a ClassLoader so it can resolve classes from
// threads that have no Java stack frame. One loader is enough — ours resolves
// every class the same way FindClass does.
static klj_val klj_Class_getClassLoader(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *loader;
    if (!loader) loader = kl_jni_new_object("java/lang/ClassLoader");
    return (klj_val){.l = loader};
}

// Class.forName(name, initialize, loader). Our FindClass never fails and there
// is no initialization to run, so this is just interning under another name.
// Note the JVM spelling: dots, not slashes.
static klj_val klj_class_by_dotted_name(const klj_val *a, int n) {
    const char *dotted = n > 0 ? klj_str(a[0].l) : NULL;
    if (!dotted) return (klj_val){0};
    char internal[224];
    size_t i = 0;
    for (; dotted[i] && i < sizeof internal - 1; i++)
        internal[i] = dotted[i] == '.' ? '/' : dotted[i];
    internal[i] = '\0';
    pthread_mutex_lock(&g_lock);
    void *c = klj_intern_class_locked(internal)->as_object;
    pthread_mutex_unlock(&g_lock);
    return (klj_val){.l = c};
}

static klj_val klj_Class_forName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    return klj_class_by_dotted_name(a, n);
}

// ClassLoader.findClass(name) — the same operation reached through the loader
// rather than through Class, and the one a guest uses when it wants a class from
// a library it has just System.load()ed. Unity's ReflectionHelper takes this
// door for the Java half of a native plugin (AVProVideo2's `Manager`), because
// the plugin's classes are not on the boot classpath.
//
// Interning is the whole of it here for the same reason forName is: FindClass
// never fails, so there is no loader delegation to model and no
// ClassNotFoundException to throw. The distinction that matters on Android —
// which ClassLoader a class came from — has no meaning against a synthetic
// registry with exactly one namespace in it.
static klj_val klj_ClassLoader_findClass(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    return klj_class_by_dotted_name(a, n);
}

// StringBuilder. Unity uses it to assemble strings it then hands back to us, so
// it needs real accumulate-and-read behaviour, not a stub. The semantics are
// fully determined by the Java API, so there is nothing invented here.
typedef struct { char *buf; size_t len, cap; } klj_strbuf;

static void klj_sb_append(klj_strbuf *sb, const char *s) {
    if (!sb || !s) return;
    size_t add = strlen(s);
    if (sb->len + add + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 64;
        while (cap < sb->len + add + 1) cap *= 2;
        sb->buf = realloc(sb->buf, cap);
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, add + 1);
    sb->len += add;
}
static klj_strbuf *klj_sb(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_val klj_SB_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz; (void)a; (void)n;
    void       *obj = kl_jni_new_object("java/lang/StringBuilder");
    klj_object *o   = klj_as_object(obj);
    o->data = calloc(1, sizeof(klj_strbuf));
    klj_sb_append(o->data, "");
    return (klj_val){.l = obj};
}
static klj_val klj_SB_toString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_strbuf *sb = klj_sb(self);
    return (klj_val){.l = kl_jni_new_string(sb && sb->buf ? sb->buf : "")};
}

// Every append overload returns `this`, which is what makes chaining work.
#define KLJ_SB_APPEND(suffix, fmt, expr)                                        \
    static klj_val klj_SB_append_##suffix(void *env, void *self,                \
                                          const klj_val *a, int n) {            \
        (void)env; (void)n;                                                     \
        char tmp[64];                                                           \
        snprintf(tmp, sizeof tmp, fmt, expr);                                   \
        klj_sb_append(klj_sb(self), tmp);                                       \
        return (klj_val){.l = self};                                            \
    }
KLJ_SB_APPEND(I, "%d",  (int)a[0].j)
KLJ_SB_APPEND(J, "%lld", (long long)a[0].j)
KLJ_SB_APPEND(C, "%c",  (char)a[0].j)
KLJ_SB_APPEND(D, "%g",  a[0].d)
KLJ_SB_APPEND(F, "%g",  (double)(float)a[0].d)
#undef KLJ_SB_APPEND
static klj_val klj_SB_append_Z(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)n;
    klj_sb_append(klj_sb(self), a[0].j ? "true" : "false");
    return (klj_val){.l = self};
}
static klj_val klj_SB_append_S(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)n;
    const char *s = klj_str(a[0].l);
    klj_sb_append(klj_sb(self), s ? s : "null");
    return (klj_val){.l = self};
}

const klj_binding klj_bind_lang[] = {

    {"java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", klj_Class_forName},
    {"java/lang/ClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;", klj_ClassLoader_findClass},

    {"java/lang/StringBuilder", "<init>",   "()V",                  klj_SB_init},
    {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;", klj_SB_toString},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", klj_SB_append_S},
    {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", klj_SB_append_I},
    {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", klj_SB_append_J},
    {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", klj_SB_append_C},
    {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", klj_SB_append_Z},
    {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", klj_SB_append_D},
    {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", klj_SB_append_F},
    {0}
};
