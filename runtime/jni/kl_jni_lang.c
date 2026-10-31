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
    if (!loader) loader = klj_host_cache(kl_jni_new_object("java/lang/ClassLoader"));
    return (klj_val){.l = loader};
}

// Class.forName(name, initialize, loader). Our FindClass interns any name but
// the ones klj_class_absent lists, and there is no initialization to run, so
// this is just interning under another name.
// Note the JVM spelling: dots, not slashes.
static klj_val klj_class_by_dotted_name(const klj_val *a, int n) {
    const char *dotted = n > 0 ? klj_str(a[0].l) : NULL;
    if (!dotted) return (klj_val){0};
    char internal[224];
    size_t i = 0;
    for (; dotted[i] && i < sizeof internal - 1; i++)
        internal[i] = dotted[i] == '.' ? '/' : dotted[i];
    internal[i] = '\0';
    // Unity's FindClass falls back to the app's class loader when JNI FindClass
    // throws, so a class the APK does not ship must be absent here too.
    if (klj_class_absent(internal)) {
        klj_raise("java/lang/ClassNotFoundException", dotted);
        return (klj_val){.l = NULL};
    }
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
// interns any name but the ones klj_class_absent lists (for those, a
// ClassNotFoundException is left pending), so there is no loader delegation to
// model. The distinction that matters on Android —
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

// Object.toString, as Java answers it: a String is itself, an exception is
// "<binary.name>: <message>" (Throwable.toString), anything else
// "<binary.name>@<hash>". Unity asks it of the exception it just caught —
// the FileNotFoundException a missing patch OBB raises (klj_ZipFile_init).
//
// It is resolved on java/lang/Object and called on whatever the object is, and
// every class without a toString of its own falls back to this row — so the
// receiver decides: a class that has its own answer (StringBuilder, Locale)
// gives that one, and for any other class the identity form is Java's default
// but may not be what the real class says (a File, a Uri, an Integer print
// their content). Those are named once each, so that a wrong string in a
// guest's log has an address.
static void klj_dotted(char *out, size_t cap, const char *cls) {
    snprintf(out, cap, "%s", cls ? cls : "java/lang/Object");
    for (char *c = out; *c; c++) if (*c == '/') *c = '.';
}
static klj_val klj_Object_toString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    if (!o) return (klj_val){.l = NULL};
    if (strcmp(o->cls, KLJ_CLASS_STRING) == 0) return (klj_val){.l = self};
    klj_impl own = klj_own_impl(o->cls, "toString", "()Ljava/lang/String;");
    if (own && own != klj_Object_toString) return own(env, self, a, n);
    char name[256], buf[1400];
    klj_dotted(name, sizeof name, o->cls);
    const char *msg = klj_throwable_message(self);
    if (!msg) {
        static char said[16][96];
        static int nsaid;
        int known = 0;
        for (int i = 0; i < nsaid; i++) if (strcmp(said[i], o->cls) == 0) { known = 1; break; }
        if (!known && nsaid < 16) {
            snprintf(said[nsaid++], sizeof said[0], "%s", o->cls);
            KLJ_LOG("Object.toString on a %s: answered with the identity form; a class that prints "
                    "its content needs a toString binding of its own", name);
        }
    }
    if (msg && *msg) snprintf(buf, sizeof buf, "%s: %s", name, msg);
    else if (msg)    snprintf(buf, sizeof buf, "%s", name);
    else             snprintf(buf, sizeof buf, "%s@%lx", name,
                              (unsigned long)((uintptr_t)self >> 4) & 0xfffffffful);
    return (klj_val){.l = kl_jni_new_string(buf)};
}
static klj_val klj_Throwable_getMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *msg = klj_throwable_message(self);
    return (klj_val){.l = msg ? kl_jni_new_string(msg) : NULL};
}

// android.util.Log.getStackTraceString(Throwable) — Unity's C# side
// (AndroidJNISafe.CheckException) asks it of every exception it converts into
// an AndroidJavaException. There is no Java stack behind ours; the first line,
// which is all a stack string's reader keys on, is the Throwable's toString.
static klj_val klj_Log_getStackTraceString(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    if (n < 1 || !a[0].l) return (klj_val){.l = kl_jni_new_string("")};
    return klj_Object_toString(env, a[0].l, NULL, 0);
}

const klj_binding klj_bind_lang[] = {

    {"android/util/Log", "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;",
     klj_Log_getStackTraceString},

    {"java/lang/Object",    "toString",   "()Ljava/lang/String;", klj_Object_toString},
    {"java/lang/Throwable", "toString",   "()Ljava/lang/String;", klj_Object_toString},
    {"java/lang/Throwable", "getMessage", "()Ljava/lang/String;", klj_Throwable_getMessage},

    {"java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    // Space Atlas VR avatar SDK: the worker thread uses the same app loader.
    {"java/lang/Thread", "getContextClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", klj_Class_forName},
    // The 1-arg form, and the same again as Unity's AndroidJavaClass spells it
    // when C# asks CallStatic<AndroidJavaObject>("forName", name): the return
    // type in the signature is the generic argument's, not Class. Arizona
    // Sunshine reaches the second through ReflectionHelper.getMethodID.
    {"java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", klj_Class_forName},
    {"java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Object;", klj_Class_forName},
    {"java/lang/ClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;", klj_ClassLoader_findClass},
    // loadClass is findClass's public face; the delegation between them is the
    // only difference on Android and there is nothing here to delegate to.
    {"java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", klj_ClassLoader_findClass},
    {"java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;Z)Ljava/lang/Class;", klj_ClassLoader_findClass},
    // Context.getClassLoader() — the same loader, reached the other way round.
    // A native library that was handed no Class of its own gets there through
    // the Context: `ActivityThread.currentActivityThread().getApplication()`,
    // then `.getClassLoader()`, then `loadClass`. libEOSSDK.so takes exactly
    // that route out of its JNI_OnLoad-cached VM. Every Context is the same
    // Context here, and our class registry has a single namespace, so all
    // three spellings answer with the one loader.
    {"android/content/Context", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"android/app/Application", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"android/app/Activity",    "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},

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
