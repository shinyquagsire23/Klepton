// Synthetic JavaVM / JNIEnv. See kl_jni.h for why this exists at all.
//
// Both tables are built from kl_jni_slots.h, which is generated from the NDK's
// jni.h. The X-macro yields two things from one list: an enum of slot indices
// (so overrides are written by *name* and the compiler resolves the index) and
// one named abort stub per slot. Nothing here hardcodes a slot number, which is
// what keeps this immune to the off-by-one class of bug.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_va.h"

static int g_permissive = 0;
void kl_jni_set_permissive(int on) { g_permissive = on; }

// Table construction is deferred to first use; the host may build objects before
// it ever touches the VM handle, so the public constructors force it too.
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static void klj_init(void);

#define KLJ_LOG(...) do { fprintf(stderr, "[jni] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------------------------------------------------------------- slot enums
#define KLJ_ENUM_ENV(idx, name) KLJ_ENV_##name = idx,
#define KLJ_ENUM_VM(idx, name)  KLJ_VM_##name  = idx,
enum { KL_JNI_SLOTS(KLJ_ENUM_ENV) };
enum { KL_JVM_SLOTS(KLJ_ENUM_VM) };

// ---------------------------------------------------------------- abort stubs
// A slot we have not implemented aborts by name. It cannot do anything else:
// we do not know the return type, so there is no value we could invent. This is
// the instrument that makes the JNI surface measurable instead of guessable.
static __attribute__((noreturn)) void klj_unimpl(const char *table, int idx, const char *name) {
    fprintf(stderr, "\n[jni] UNIMPLEMENTED %s slot %d: %s\n", table, idx, name);
    fprintf(stderr, "[jni] this is an M4 work item — the guest wants it, so implement it.\n");
    kl_jni_report(stderr);
    abort();
}
#define KLJ_STUB_ENV(idx, name) \
    static __attribute__((noreturn)) void klj_env_stub_##name(void) { klj_unimpl("JNIEnv", idx, #name); }
#define KLJ_STUB_VM(idx, name) \
    static __attribute__((noreturn)) void klj_vm_stub_##name(void) { klj_unimpl("JavaVM", idx, #name); }
KL_JNI_SLOTS(KLJ_STUB_ENV)
KL_JVM_SLOTS(KLJ_STUB_VM)

// ---------------------------------------------------------------- registries
#define KLJ_MAX_CLASSES 512
#define KLJ_MAX_NATIVES 1024
#define KLJ_MAX_WANTED  1024

// Every jobject we hand the guest is one of these. The guest can only ever get
// an object from us, so tagging them means GetObjectClass has a real answer
// instead of a guess, and a jobject arriving from somewhere unexpected is
// caught by the magic rather than dereferenced blindly.
#define KLJ_OBJ_MAGIC 0x4B4C4A4FU   /* 'KLJO' */
#define KLJ_MAX_OBJECTS 8192
typedef struct klj_object {
    uint32_t    magic;
    const char *cls;    // interned class name
    void       *data;   // java/lang/String -> char*; java/lang/Class -> klj_class*
} klj_object;

static const char KLJ_CLASS_CLASS[]  = "java/lang/Class";
static const char KLJ_CLASS_STRING[] = "java/lang/String";

typedef struct { char name[224]; klj_object *as_object; } klj_class;
static klj_class g_classes[KLJ_MAX_CLASSES];
static unsigned  g_nclasses;

static klj_object g_objects[KLJ_MAX_OBJECTS];
static unsigned   g_nobjects;

// Objects are never freed. They are startup-lifetime and there is no GC to
// coordinate with; a free list here would buy nothing and cost use-after-free.
static klj_object *klj_alloc_object_locked(const char *cls, void *data) {
    if (g_nobjects == KLJ_MAX_OBJECTS) { KLJ_LOG("object pool exhausted"); abort(); }
    klj_object *o = &g_objects[g_nobjects++];
    *o = (klj_object){KLJ_OBJ_MAGIC, cls, data};
    return o;
}

static klj_object *klj_as_object(void *p) {
    klj_object *o = p;
    return (o && o->magic == KLJ_OBJ_MAGIC) ? o : NULL;
}

typedef struct {
    const char *cls, *name, *sig;
    void       *fn;
} klj_native;
static klj_native g_natives[KLJ_MAX_NATIVES];
static unsigned   g_nnatives;

// Every method/field id the guest asked for. The address of an entry *is* the
// jmethodID/jfieldID we hand back, so identity is (class, name, signature) —
// which is exactly what JNI guarantees.
typedef struct {
    const char *cls;
    char        name[160], sig[224];
    char        kind;   // m=method M=static method f=field F=static field
} klj_wanted;
static klj_wanted g_wanted[KLJ_MAX_WANTED];
static unsigned   g_nwanted;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Interning gives every class name one stable address, so a klj_object can hold
// a bare `const char *cls` that stays valid forever, and the "have we seen this
// before" log dedupe falls out of the same lookup.
static klj_class *klj_intern_class_locked(const char *name) {
    for (unsigned i = 0; i < g_nclasses; i++)
        if (strcmp(g_classes[i].name, name) == 0) return &g_classes[i];
    if (g_nclasses == KLJ_MAX_CLASSES) { KLJ_LOG("class table full at %s", name); abort(); }
    klj_class *c = &g_classes[g_nclasses++];
    snprintf(c->name, sizeof c->name, "%s", name);
    // A jclass is a jobject in JNI, so it gets the same representation as one.
    c->as_object = klj_alloc_object_locked(KLJ_CLASS_CLASS, c);
    KLJ_LOG("FindClass  %s", c->name);
    return c;
}

// Accepts the jclass the guest was given — i.e. a java/lang/Class object.
static const char *klj_class_name(void *clazz) {
    klj_object *o = klj_as_object(clazz);
    if (o && strcmp(o->cls, KLJ_CLASS_CLASS) == 0) return ((klj_class *)o->data)->name;
    return "<unknown-jclass>";
}

// ---- object/string construction, for the host side of the boundary ----
void *kl_jni_new_object(const char *class_name) {
    pthread_once(&g_init_once, klj_init);
    pthread_mutex_lock(&g_lock);
    klj_class  *c = klj_intern_class_locked(class_name);
    klj_object *o = klj_alloc_object_locked(c->name, NULL);
    pthread_mutex_unlock(&g_lock);
    return o;
}

void *kl_jni_new_string(const char *utf8) {
    pthread_once(&g_init_once, klj_init);
    pthread_mutex_lock(&g_lock);
    klj_object *o = klj_alloc_object_locked(KLJ_CLASS_STRING, utf8 ? strdup(utf8) : NULL);
    pthread_mutex_unlock(&g_lock);
    return o;
}

// Unwrap a jstring. Returns NULL for anything that is not one, which the
// callers report rather than dereference.
static const char *klj_str(void *s) {
    klj_object *o = klj_as_object(s);
    if (o && strcmp(o->cls, KLJ_CLASS_STRING) == 0) return o->data;
    if (o) KLJ_LOG("expected a java/lang/String, got %s", o->cls);
    else if (s) KLJ_LOG("expected a java/lang/String, got an untagged pointer %p", s);
    return NULL;
}

void *kl_jni_native(const char *cls, const char *name, const char *sig) {
    for (unsigned i = 0; i < g_nnatives; i++)
        if (strcmp(g_natives[i].cls, cls) == 0 && strcmp(g_natives[i].name, name) == 0 &&
            (!sig || strcmp(g_natives[i].sig, sig) == 0))
            return g_natives[i].fn;
    return NULL;
}

void kl_jni_report(FILE *out) {
    fprintf(out, "\n--- JNI surface actually exercised ---\n");
    fprintf(out, "  classes found: %u\n", g_nclasses);
    for (unsigned i = 0; i < g_nclasses; i++) fprintf(out, "    %s\n", g_classes[i].name);
    fprintf(out, "  natives registered: %u\n", g_nnatives);
    for (unsigned i = 0; i < g_nnatives; i++)
        fprintf(out, "    %s.%s%s -> %p\n", g_natives[i].cls, g_natives[i].name,
                g_natives[i].sig, g_natives[i].fn);
    fprintf(out, "  ids requested: %u\n", g_nwanted);
    for (unsigned i = 0; i < g_nwanted; i++)
        fprintf(out, "    [%c] %s.%s%s\n", g_wanted[i].kind, g_wanted[i].cls,
                g_wanted[i].name, g_wanted[i].sig);
    fprintf(out, "---\n");
}

// ---------------------------------------------------------------- JNIEnv impl
// Implemented here are only the operations whose correct answer is forced, not
// guessed. Reference management is the identity function because we have no GC
// and a jobject is already a raw pointer; exception state is always "clear"
// because nothing in this runtime can throw. Anything that would require us to
// invent Java semantics stays an abort stub.

static void *klj_GetVersion(void *env) { (void)env; return (void *)(uintptr_t)KL_JNI_VERSION_1_6; }

static void *klj_FindClass(void *env, const char *name) {
    (void)env;
    pthread_mutex_lock(&g_lock);
    void *c = klj_intern_class_locked(name)->as_object;
    pthread_mutex_unlock(&g_lock);
    return c;
}

static void *klj_GetObjectClass(void *env, void *obj) {
    (void)env;
    klj_object *o = klj_as_object(obj);
    if (!o) {
        KLJ_LOG("GetObjectClass on an untagged pointer %p — every jobject the guest "
                "holds should have come from us", obj);
        kl_jni_report(stderr);
        abort();
    }
    pthread_mutex_lock(&g_lock);
    void *c = klj_intern_class_locked(o->cls)->as_object;
    pthread_mutex_unlock(&g_lock);
    return c;
}

static void *klj_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void  klj_ExceptionDescribe(void *env) { (void)env; }
static void  klj_ExceptionClear(void *env)    { (void)env; }
static kl_jint klj_ExceptionCheck(void *env)  { (void)env; return 0; }

static __attribute__((noreturn)) void klj_FatalError(void *env, const char *msg) {
    (void)env;
    fprintf(stderr, "\n[jni] FatalError from guest: %s\n", msg ? msg : "(null)");
    kl_jni_report(stderr);
    abort();
}

static kl_jint klj_PushLocalFrame(void *env, kl_jint cap) { (void)env; (void)cap; return 0; }
static void *klj_PopLocalFrame(void *env, void *result)   { (void)env; return result; }
static kl_jint klj_EnsureLocalCapacity(void *env, kl_jint c) { (void)env; (void)c; return 0; }
static void *klj_ref_identity(void *env, void *obj)       { (void)env; return obj; }
static void  klj_ref_release(void *env, void *obj)        { (void)env; (void)obj; }
static kl_jint klj_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

// The class hierarchy the guest can observe. Only the edges that matter: Unity
// asks whether the Context it was handed is really an Activity, and on device it
// is — UnityPlayerActivity is what the manifest declares. Answering by exact
// name match would say "no" and send the engine down its no-Activity path.
static const struct { const char *cls, *super; } g_supers[] = {
    {"com/unity3d/player/UnityPlayerActivity", "android/app/Activity"},
    {"android/app/Activity",                   "android/view/ContextThemeWrapper"},
    {"android/view/ContextThemeWrapper",       "android/content/ContextWrapper"},
    {"android/content/ContextWrapper",         "android/content/Context"},
    {NULL, NULL},
};

static kl_jint klj_IsInstanceOf(void *env, void *obj, void *clazz) {
    (void)env;
    if (!obj) return 1;                       // null is an instance of everything
    klj_object *o = klj_as_object(obj);
    if (!o) return 0;
    const char *want = klj_class_name(clazz);
    if (strcmp(want, "java/lang/Object") == 0) return 1;

    for (const char *cur = o->cls; cur; ) {
        if (strcmp(cur, want) == 0) return 1;
        const char *next = NULL;
        for (int i = 0; g_supers[i].cls; i++)
            if (strcmp(g_supers[i].cls, cur) == 0) { next = g_supers[i].super; break; }
        cur = next;
    }
    return 0;
}

// Strings. A jstring never crosses into real Java, so we are free to define the
// representation, and the useful definition is the one the guest already wants
// back: a NUL-terminated modified-UTF8 buffer. GetStringUTFChars is then the
// identity with isCopy=false, and Release is a no-op — no conversion, no
// ownership question. Modified UTF-8 differs from UTF-8 only in how it encodes
// U+0000 and non-BMP characters, neither of which appears in the paths and
// identifiers this is used for.
static kl_jint klj_GetStringUTFLength(void *env, void *jstr) {
    (void)env;
    const char *s = klj_str(jstr);
    return s ? (kl_jint)strlen(s) : 0;
}

// UTF-16 code units, which is *not* the byte count once a path leaves ASCII.
static kl_jint klj_GetStringLength(void *env, void *jstr) {
    (void)env;
    const char *s = klj_str(jstr);
    kl_jint units = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ) {
        if (*p < 0x80)        p += 1, units += 1;
        else if (*p < 0xE0)   p += 2, units += 1;
        else if (*p < 0xF0)   p += 3, units += 1;
        else                  p += 4, units += 2;   // surrogate pair
    }
    return units;
}

static const char *klj_GetStringUTFChars(void *env, void *jstr, uint8_t *isCopy) {
    (void)env;
    if (isCopy) *isCopy = 0;   // we hand back the buffer itself, so no copy
    return klj_str(jstr);
}
static void klj_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {
    (void)env; (void)jstr; (void)chars;   // nothing was copied, so nothing to free
}
static void *klj_NewStringUTF(void *env, const char *s) {
    (void)env;
    return s ? kl_jni_new_string(s) : NULL;
}

static kl_jint klj_RegisterNatives(void *env, void *clazz, const kl_jni_method *m, kl_jint n) {
    (void)env;
    const char *cls = klj_class_name(clazz);
    pthread_mutex_lock(&g_lock);
    for (kl_jint i = 0; i < n; i++) {
        if (g_nnatives == KLJ_MAX_NATIVES) { KLJ_LOG("native table full"); abort(); }
        g_natives[g_nnatives++] = (klj_native){cls, m[i].name, m[i].signature, m[i].fnPtr};
        KLJ_LOG("RegisterNatives  %s.%s%s -> %p", cls, m[i].name, m[i].signature, m[i].fnPtr);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static kl_jint klj_UnregisterNatives(void *env, void *clazz) { (void)env; (void)clazz; return 0; }

static kl_jint klj_GetJavaVM(void *env, void **vm) { (void)env; *vm = kl_jni_vm(); return 0; }

// The measurement instrument. Every id the guest wants is recorded; in strict
// mode the first unknown one stops the run, which is the bring-up loop from
// PLANNING §6 M4. Permissive mode hands back a synthetic id so a single run
// collects the whole batch of lookups an init path performs.
static void *klj_want(void *clazz, const char *name, const char *sig, char kind) {
    const char *cls = klj_class_name(clazz);
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_nwanted; i++)
        if (g_wanted[i].cls == cls && g_wanted[i].kind == kind &&
            strcmp(g_wanted[i].name, name) == 0 && strcmp(g_wanted[i].sig, sig) == 0) {
            pthread_mutex_unlock(&g_lock);
            return &g_wanted[i];
        }
    if (g_nwanted == KLJ_MAX_WANTED) { KLJ_LOG("id table full"); abort(); }
    klj_wanted *w = &g_wanted[g_nwanted++];
    w->cls = cls; w->kind = kind;
    snprintf(w->name, sizeof w->name, "%s", name);
    snprintf(w->sig, sizeof w->sig, "%s", sig);
    pthread_mutex_unlock(&g_lock);

    KLJ_LOG("%s  %s.%s%s", kind == 'm' ? "GetMethodID      " :
                           kind == 'M' ? "GetStaticMethodID" :
                           kind == 'f' ? "GetFieldID       " : "GetStaticFieldID ",
            cls, name, sig);
    // Deliberately never fatal. A lookup is a *measurement*, and the guest looks
    // up ids it may never call — aborting here would stop on methods that do not
    // actually need implementing. The failure belongs at the call, where
    // klj_call knows an implementation is genuinely missing.
    return w;
}
static void *klj_GetMethodID(void *e, void *c, const char *n, const char *s)       { (void)e; return klj_want(c, n, s, 'm'); }
static void *klj_GetStaticMethodID(void *e, void *c, const char *n, const char *s) { (void)e; return klj_want(c, n, s, 'M'); }
static void *klj_GetFieldID(void *e, void *c, const char *n, const char *s)        { (void)e; return klj_want(c, n, s, 'f'); }
static void *klj_GetStaticFieldID(void *e, void *c, const char *n, const char *s)  { (void)e; return klj_want(c, n, s, 'F'); }

// ------------------------------------------------------- Java method dispatch
// The guest calls Java through Call<Type>Method[V|A]. It is C++, so its inline
// jni.h wrappers va_start and hand us the *V* form — meaning the fourth argument
// is a guest AAPCS64 va_list (a 32-byte descriptor, passed by reference because
// it exceeds 16 bytes). Same shape as the libc variadics in kl_va.h, and it
// reuses the same kl_va_gp/kl_va_fp accessors.
//
// Unlike printf there is no format string to walk — but there does not need to
// be. A jmethodID *is* the klj_wanted entry we created at GetMethodID time, so
// the JNI signature is already in hand and it types the arguments exactly.

typedef union { uint64_t j; double d; void *l; } klj_val;
typedef klj_val (*klj_impl)(void *env, void *self, const klj_val *argv, int argc);

typedef struct { const char *cls, *name, *sig; klj_impl fn; } klj_binding;
static const klj_binding g_bindings[];   // defined below, after the impls

#define KLJ_MAX_ARGS 16

// Walk the argument list of a JNI signature, pulling each one from the guest
// va_list in the right register bank. Returns argc, or -1 on a malformed sig.
static int klj_decode_args(const char *sig, kl_va *va, klj_val *argv) {
    if (!sig || *sig != '(') return -1;
    int argc = 0;
    for (const char *p = sig + 1; *p && *p != ')'; ) {
        if (argc == KLJ_MAX_ARGS) return -1;
        while (*p == '[') p++;                       // arrays are references
        switch (*p) {
        case 'L':                                     // fully-qualified object
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            argv[argc++].l = (void *)(uintptr_t)kl_va_gp(va);
            break;
        // Z/B/C/S/I are all promoted to int by the varargs rules, and AAPCS64
        // gives each one a full GP slot, so they read identically here.
        case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J':
            argv[argc++].j = kl_va_gp(va);
            p++;
            break;
        case 'F':                                     // float promotes to double
        case 'D':
            argv[argc++].d = kl_va_fp(va);
            p++;
            break;
        default:
            return -1;
        }
    }
    return argc;
}

// The character after ')' — used only to check the guest picked the Call slot
// that matches the signature it registered.
static char klj_return_kind(const char *sig) {
    const char *p = sig ? strchr(sig, ')') : NULL;
    if (!p) return '?';
    p++;
    return (*p == '[') ? 'L' : *p;
}

static klj_val klj_call(void *env, void *self, void *mid, kl_va *va, char want) {
    klj_val zero = {0};
    klj_wanted *w = mid;
    if (!w || w < g_wanted || w >= g_wanted + KLJ_MAX_WANTED) {
        KLJ_LOG("Call*Method with a jmethodID we never issued (%p)", mid);
        kl_jni_report(stderr);
        abort();
    }

    char have = klj_return_kind(w->sig);
    if (want != '?' && have != want)
        KLJ_LOG("WARNING %s.%s%s called through a '%c' slot but returns '%c'",
                w->cls, w->name, w->sig, want, have);

    for (const klj_binding *b = g_bindings; b->cls; b++) {
        if (strcmp(b->cls, w->cls) || strcmp(b->name, w->name) || strcmp(b->sig, w->sig))
            continue;
        klj_val argv[KLJ_MAX_ARGS];
        int argc = klj_decode_args(w->sig, va, argv);
        if (argc < 0) {
            KLJ_LOG("cannot decode signature %s for %s.%s", w->sig, w->cls, w->name);
            abort();
        }
        return b->fn(env, self, argv, argc);
    }

    KLJ_LOG("no host implementation for %s.%s%s", w->cls, w->name, w->sig);
    if (!g_permissive) {
        fprintf(stderr, "[jni] this is an M4 work item — add it to g_bindings.\n");
        kl_jni_report(stderr);
        abort();
    }
    return zero;
}

// One entry point per return kind. The static forms differ only in that `self`
// is the jclass rather than an instance, which the implementations ignore.
#define KLJ_CALL_V(Name, Ret, Kind, Expr)                                            \
    static Ret klj_Call##Name##MethodV(void *env, void *self, void *mid, kl_va *va) { \
        klj_val r = klj_call(env, self, mid, va, Kind); (void)r; return Expr;          \
    }                                                                                 \
    static Ret klj_CallStatic##Name##MethodV(void *env, void *cls, void *mid, kl_va *va) { \
        klj_val r = klj_call(env, cls, mid, va, Kind); (void)r; return Expr;           \
    }
KLJ_CALL_V(Object,  void *,   'L', r.l)
KLJ_CALL_V(Boolean, uint8_t,  'Z', (uint8_t)r.j)
KLJ_CALL_V(Byte,    int8_t,   'B', (int8_t)r.j)
KLJ_CALL_V(Char,    uint16_t, 'C', (uint16_t)r.j)
KLJ_CALL_V(Short,   int16_t,  'S', (int16_t)r.j)
KLJ_CALL_V(Int,     kl_jint,  'I', (kl_jint)r.j)
KLJ_CALL_V(Long,    int64_t,  'J', (int64_t)r.j)
KLJ_CALL_V(Float,   float,    'F', (float)r.d)
KLJ_CALL_V(Double,  double,   'D', r.d)
#undef KLJ_CALL_V
static void klj_CallVoidMethodV(void *env, void *self, void *mid, kl_va *va) {
    klj_call(env, self, mid, va, 'V');
}
static void klj_CallStaticVoidMethodV(void *env, void *cls, void *mid, kl_va *va) {
    klj_call(env, cls, mid, va, 'V');
}

// A constructor is just a method named <init>; it returns the new object rather
// than the 'V' its signature claims, so the slot check is skipped.
static void *klj_NewObjectV(void *env, void *clazz, void *mid, kl_va *va) {
    return klj_call(env, clazz, mid, va, '?').l;
}

// ------------------------------------------------------------ Java class impls
// Host implementations of the Java methods the guest actually calls. This stays
// here while it is small; it splits into its own file once M4 fills it out.

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
static klj_val klj_Class_forName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
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

// ---- assets ----
// This is the path the M3 measurement predicted: with no AAssetManager_* import,
// assets reach Unity over JNI instead — Context.getAssets() -> AssetManager.open()
// -> InputStream -> Scanner. We serve it from the unpacked APK on disk.
static const char *g_assets_dir = "beatsaber/assets";
void kl_jni_set_assets_dir(const char *dir) { g_assets_dir = dir; }

static klj_val klj_singleton(const char *cls, void **slot) {
    if (!*slot) *slot = kl_jni_new_object(cls);
    return (klj_val){.l = *slot};
}

static klj_val klj_Activity_getIntent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *intent;
    return klj_singleton("android/content/Intent", &intent);
}
// A normally-launched activity has no extras, so null is the truthful answer
// here rather than a shortcut — Unity treats it as "no launch arguments".
static klj_val klj_Intent_getExtras(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}
static klj_val klj_Context_getAssets(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *am;
    return klj_singleton("android/content/res/AssetManager", &am);
}

// InputStream carries the whole asset in memory: assets read this way are small
// config blobs, and it makes Scanner a pure string walk.
typedef struct { char *data; size_t len, pos; } klj_stream;

static klj_val klj_AssetManager_open(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *rel = n > 0 ? klj_str(a[0].l) : NULL;
    if (!rel) return (klj_val){.l = NULL};

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_assets_dir, rel);
    FILE *f = fopen(path, "rb");
    KLJ_LOG("AssetManager.open(\"%s\") -> %s", rel, f ? path : "MISSING");
    if (!f) return (klj_val){.l = NULL};   // guest catches IOException

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    klj_stream *st = calloc(1, sizeof *st);
    st->data = malloc((size_t)sz + 1);
    st->len  = fread(st->data, 1, (size_t)sz, f);
    st->data[st->len] = '\0';
    fclose(f);

    void *obj = kl_jni_new_object("java/io/InputStream");
    klj_as_object(obj)->data = st;
    return (klj_val){.l = obj};
}

// Scanner, only as far as Unity uses it: wrap a stream, set a delimiter, pull
// tokens. Java's delimiter is a *regex*, and we do not have one — so we handle
// the input-anchor idioms exactly and treat anything else as a literal, warning
// when that is a guess. Unity slurps boot.config with "\z"; if we simply
// searched for that as a literal it would not be found and we would return the
// whole input anyway — the right answer for the wrong reason. Naming the case
// keeps the next delimiter from splitting silently in the wrong place.
static int klj_delim_whole_input(const char *d) {
    return strcmp(d, "\\A") == 0 || strcmp(d, "\\z") == 0 || strcmp(d, "\\Z") == 0;
}
typedef struct { klj_stream *st; char delim[32]; } klj_scanner;

static klj_val klj_Scanner_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_object *in = n > 0 ? klj_as_object(a[0].l) : NULL;
    klj_scanner *sc = calloc(1, sizeof *sc);
    sc->st = in ? in->data : NULL;
    snprintf(sc->delim, sizeof sc->delim, "%s", "\n");
    void *obj = kl_jni_new_object("java/util/Scanner");
    klj_as_object(obj)->data = sc;
    return (klj_val){.l = obj};
}
static klj_val klj_Scanner_useDelimiter(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object  *o  = klj_as_object(self);
    klj_scanner *sc = o ? o->data : NULL;
    const char  *d  = n > 0 ? klj_str(a[0].l) : NULL;
    if (sc && d) snprintf(sc->delim, sizeof sc->delim, "%s", d);
    KLJ_LOG("Scanner.useDelimiter(\"%s\")%s", d ? d : "(null)",
            d && !klj_delim_whole_input(d) && strpbrk(d, "\\[](){}*+?|^$.")
                ? "  WARNING: regex delimiter treated as a literal" : "");
    return (klj_val){.l = self};
}
static klj_val klj_Scanner_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object  *o  = klj_as_object(self);
    klj_scanner *sc = o ? o->data : NULL;
    if (!sc || !sc->st || sc->st->pos >= sc->st->len)
        return (klj_val){.l = NULL};   // guest catches NoSuchElementException

    klj_stream *st   = sc->st;
    const char *rest = st->data + st->pos;
    const char *end  = klj_delim_whole_input(sc->delim) ? NULL : strstr(rest, sc->delim);
    size_t take = end ? (size_t)(end - rest) : st->len - st->pos;

    char *tok = malloc(take + 1);
    memcpy(tok, rest, take);
    tok[take] = '\0';
    st->pos += take + (end ? strlen(sc->delim) : 0);

    KLJ_LOG("Scanner.next() -> %zu bytes", take);
    void *s = kl_jni_new_string(tok);
    free(tok);
    return (klj_val){.l = s};
}

static const klj_binding g_bindings[] = {
    {"java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", klj_Class_forName},

    {"java/lang/StringBuilder", "<init>",   "()V",                  klj_SB_init},
    {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;", klj_SB_toString},
    {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", klj_SB_append_S},
    {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", klj_SB_append_I},
    {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", klj_SB_append_J},
    {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", klj_SB_append_C},
    {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", klj_SB_append_Z},
    {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", klj_SB_append_D},
    {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", klj_SB_append_F},

    {"android/app/Activity",   "getIntent",  "()Landroid/content/Intent;", klj_Activity_getIntent},
    {"android/content/Intent", "getExtras",  "()Landroid/os/Bundle;",      klj_Intent_getExtras},
    {"android/content/Context", "getAssets", "()Landroid/content/res/AssetManager;", klj_Context_getAssets},
    {"android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", klj_AssetManager_open},
    {"java/util/Scanner", "<init>",        "(Ljava/io/InputStream;Ljava/lang/String;)V", klj_Scanner_init},
    {"java/util/Scanner", "useDelimiter",  "(Ljava/lang/String;)Ljava/util/Scanner;",    klj_Scanner_useDelimiter},
    {"java/util/Scanner", "next",          "()Ljava/lang/String;",                       klj_Scanner_next},

    {NULL, NULL, NULL, NULL},
};

// ---------------------------------------------------------------- JavaVM impl
static void *g_env_vtable[KL_JNI_SLOTS_COUNT];
static void *g_vm_vtable[KL_JVM_SLOTS_COUNT];

// A JNIEnv is per-thread by contract, and a thread running guest code needs the
// bionic stack canary in TSD slot 5 before it executes anything (S0.1), so the
// attach path is also where kl_thread_init() belongs.
typedef struct { void *functions; } klj_env_t;
static pthread_key_t  g_env_key;
static void klj_build_tables(void);
static void klj_init(void) {
    pthread_key_create(&g_env_key, free);
    klj_build_tables();
}

void *kl_jni_env(void) {
    pthread_once(&g_init_once, klj_init);
    klj_env_t *e = pthread_getspecific(g_env_key);
    if (!e) {
        e = calloc(1, sizeof *e);
        e->functions = g_env_vtable;
        pthread_setspecific(g_env_key, e);
        kl_thread_init();
    }
    return e;
}

void *kl_jni_vm(void) {
    pthread_once(&g_init_once, klj_init);
    static void *vm = g_vm_vtable;
    return &vm;
}

static kl_jint klj_AttachCurrentThread(void *vm, void **penv, void *args) {
    (void)vm; (void)args;
    *penv = kl_jni_env();
    return 0;
}
static kl_jint klj_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static kl_jint klj_GetEnv(void *vm, void **penv, kl_jint version) {
    (void)vm; (void)version;
    *penv = kl_jni_env();
    return 0;
}
static kl_jint klj_DestroyJavaVM(void *vm) { (void)vm; return 0; }

// ---------------------------------------------------------------- table build
static void klj_build_tables(void) {
#define KLJ_FILL_ENV(idx, name) g_env_vtable[idx] = (void *)klj_env_stub_##name;
#define KLJ_FILL_VM(idx, name)  g_vm_vtable[idx]  = (void *)klj_vm_stub_##name;
    KL_JNI_SLOTS(KLJ_FILL_ENV)
    KL_JVM_SLOTS(KLJ_FILL_VM)
#undef KLJ_FILL_ENV
#undef KLJ_FILL_VM

#define ENV(name, fn) g_env_vtable[KLJ_ENV_##name] = (void *)(fn)
    ENV(GetVersion,           klj_GetVersion);
    ENV(FindClass,            klj_FindClass);
    ENV(GetObjectClass,       klj_GetObjectClass);
    ENV(ExceptionOccurred,    klj_ExceptionOccurred);
    ENV(ExceptionDescribe,    klj_ExceptionDescribe);
    ENV(ExceptionClear,       klj_ExceptionClear);
    ENV(ExceptionCheck,       klj_ExceptionCheck);
    ENV(FatalError,           klj_FatalError);
    ENV(PushLocalFrame,       klj_PushLocalFrame);
    ENV(PopLocalFrame,        klj_PopLocalFrame);
    ENV(EnsureLocalCapacity,  klj_EnsureLocalCapacity);
    ENV(NewGlobalRef,         klj_ref_identity);
    ENV(NewLocalRef,          klj_ref_identity);
    ENV(NewWeakGlobalRef,     klj_ref_identity);
    ENV(DeleteGlobalRef,      klj_ref_release);
    ENV(DeleteLocalRef,       klj_ref_release);
    ENV(DeleteWeakGlobalRef,  klj_ref_release);
    ENV(IsSameObject,         klj_IsSameObject);
    ENV(IsInstanceOf,         klj_IsInstanceOf);
    ENV(NewStringUTF,         klj_NewStringUTF);
    ENV(GetStringLength,      klj_GetStringLength);
    ENV(GetStringUTFLength,   klj_GetStringUTFLength);
    ENV(GetStringUTFChars,    klj_GetStringUTFChars);
    ENV(ReleaseStringUTFChars, klj_ReleaseStringUTFChars);
    ENV(RegisterNatives,      klj_RegisterNatives);
    ENV(UnregisterNatives,    klj_UnregisterNatives);
    ENV(GetJavaVM,            klj_GetJavaVM);
    ENV(GetMethodID,          klj_GetMethodID);
    ENV(GetStaticMethodID,    klj_GetStaticMethodID);
    ENV(GetFieldID,           klj_GetFieldID);
    ENV(GetStaticFieldID,     klj_GetStaticFieldID);

    // Only the V forms: the guest is C++, so its inline jni.h wrappers va_start
    // and call these. The plain varargs forms would need kl_va_thunks.S entry
    // points to materialise a va_list; they stay abort stubs until the trace
    // shows something actually calling one.
#define ENVCALL(Name) \
    ENV(Call##Name##MethodV, klj_Call##Name##MethodV); \
    ENV(CallStatic##Name##MethodV, klj_CallStatic##Name##MethodV)
    ENVCALL(Object); ENVCALL(Boolean); ENVCALL(Byte);  ENVCALL(Char);
    ENVCALL(Short);  ENVCALL(Int);     ENVCALL(Long);  ENVCALL(Float);
    ENVCALL(Double); ENVCALL(Void);
#undef ENVCALL
    ENV(NewObjectV, klj_NewObjectV);
#undef ENV

#define VM(name, fn) g_vm_vtable[KLJ_VM_##name] = (void *)(fn)
    VM(AttachCurrentThread,         klj_AttachCurrentThread);
    VM(AttachCurrentThreadAsDaemon, klj_AttachCurrentThread);
    VM(DetachCurrentThread,         klj_DetachCurrentThread);
    VM(GetEnv,                      klj_GetEnv);
    VM(DestroyJavaVM,               klj_DestroyJavaVM);
#undef VM
}
