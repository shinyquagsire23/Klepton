// Synthetic JavaVM / JNIEnv. See kl_jni.h for why this exists at all.
//
// Both tables are built from kl_jni_slots.h, which is generated from the NDK's
// jni.h. The X-macro yields two things from one list: an enum of slot indices
// (so overrides are written by *name* and the compiler resolves the index) and
// one named abort stub per slot. Nothing here hardcodes a slot number, which is
// what keeps this immune to the off-by-one class of bug.
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_egl.h"
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
    kl_egl_report(stderr);
    kl_fatal_prepare(); abort();
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
#define KLJ_MAX_OBJECTS (8192*16)   // parenthesised: a bare 8192*16 turns
                                    // "x % KLJ_MAX_OBJECTS" into (x%8192)*16
typedef struct klj_object {
    uint32_t    magic;
    uint32_t    pinned; // global-ref count, or 1 forever for interned class
                        // objects and host singletons: DeleteLocalRef and
                        // PopLocalFrame must not recycle it while > 0
    const char *cls;    // interned class name
    void       *data;   // java/lang/String -> char*; java/lang/Class -> klj_class*
} klj_object;

// The two object kinds the *host* constructs rather than merely hands back: a
// bitter/jnibridge proxy, and the java.lang.reflect.Method describing what to
// invoke on it. Declared here because FromReflectedMethod needs the Method
// description long before the JNIBridge block that builds one.
#define KLJ_CLASS_PROXY  "bitter/jnibridge/JNIBridge$Proxy"
#define KLJ_CLASS_METHOD "java/lang/reflect/Method"
#define KLJ_CLASS_FIELD  "java/lang/reflect/Field"
typedef struct { const char *cls, *name, *sig; int is_static; } klj_method_obj;

// A boxed primitive or a SharedPreferences value — the same shape serves both,
// because a preference is exactly "a typed scalar with a key". Declared up here
// rather than beside the preferences code because the Choreographer needs to box
// a long long before that file section.
typedef struct {
    char   *key;
    char    kind;     // 'S' string, 'I' 'J' 'Z' integral, 'F' float, '-' pending removal
    char   *sval;
    int64_t ival;
    float   fval;
} klj_pref;
typedef struct { const char *cls, *name, *sig; int is_static; } klj_field_obj;
static void *klj_class_object(const char *class_name);

static const char KLJ_CLASS_CLASS[]  = "java/lang/Class";
static const char KLJ_CLASS_STRING[] = "java/lang/String";

typedef struct { char name[224]; klj_object *as_object; } klj_class;
static klj_class g_classes[KLJ_MAX_CLASSES];
static unsigned  g_nclasses;

static klj_object g_objects[KLJ_MAX_OBJECTS];
static unsigned   g_nobjects;

// Local references are recycled, lazily. The loading-pace runs showed the
// pool is not startup-lifetime after all: the guest leaks ~3 objects per
// rendered frame and exhausted 131072 slots at a deterministic swap 43582.
// Two reclamation paths, matching JNI semantics:
//
//   - DeleteLocalRef retires the slot;
//   - PushLocalFrame/PopLocalFrame bracket a batch: every object allocated
//     while a frame is open on this thread is retired at pop, except the
//     result, which is handed to the parent frame. Frames are per-thread;
//     native code on arbitrary guest threads can open them independently.
//
// Retired slots are NOT reused immediately. The guest (IL2CPP built against
// Android, where a local ref is just a pointer and outliving its frame is
// invisible) keeps using some objects past their pop — observed: the int[]
// from InputDevice.getDeviceIds() retired at a pop and dereferenced a moment
// later. Retired slots therefore sit in a FIFO quarantine; only when it
// exceeds KLJ_QUARANTINE entries is the oldest slot actually recycled, and a
// jstring's strdup'd bytes are freed at that point, not at retire. 8192
// entries at the observed ~3 retires/frame is a ~2700-frame grace period.
//
// pinned is a global-ref COUNT (NewGlobalRef can hand out several handles to
// one object); interned class objects and host singletons (the Activity)
// start at 1 and effectively never reach 0. Class objects are additionally
// exempt from recycling outright: klj_class interning keeps as_object
// forever, so freeing one hands its slot to another class's object while
// FindClass keeps returning the stale pointer (observed: JNIBridge's class
// object retired at a PopLocalFrame after the guest's NewGlobalRef/
// DeleteGlobalRef pair, the slot reused for StringBuilder, and the next
// FindClass("bitter/jnibridge/JNIBridge") answered StringBuilder's class).
#define KLJ_QUARANTINE 8192
static unsigned g_retired[KLJ_MAX_OBJECTS];
static unsigned g_retired_head, g_retired_tail;   // ring; head = oldest
static unsigned g_retired_n;
static uint64_t g_stat_alloc, g_stat_delete, g_stat_pop_freed;

typedef struct klj_frame {
    struct klj_frame *parent;
    klj_object      **objs;
    unsigned          n, cap;
} klj_frame;
static __thread klj_frame *t_frame;

static void klj_frame_record(klj_object *o) {
    klj_frame *f = t_frame;
    if (!f) return;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->objs = realloc(f->objs, f->cap * sizeof *f->objs);
    }
    f->objs[f->n++] = o;
}

// Remove o from every frame open on this thread. Needed because a recycled
// slot is handed out again while an outer frame still lists the old owner —
// without this, that frame's pop retires the NEW occupant. Local refs are
// thread-local in JNI, so the current thread's chain is the only place the
// object can be listed.
static void klj_frame_forget(klj_object *o) {
    for (klj_frame *f = t_frame; f; f = f->parent)
        for (unsigned i = 0; i < f->n; i++)
            if (f->objs[i] == o) f->objs[i] = NULL;
}

// Caller holds g_lock. Pinned objects, class objects, and non-objects are
// left alone.
static void klj_retire_object_locked(klj_object *o) {
    if (!o || o->magic != KLJ_OBJ_MAGIC || o->pinned) return;
    if (strcmp(o->cls, KLJ_CLASS_CLASS) == 0) return;
    klj_frame_forget(o);
    o->magic = 0;
    g_retired[g_retired_tail] = (unsigned)(o - g_objects);
    g_retired_tail = (g_retired_tail + 1) % KLJ_MAX_OBJECTS;
    g_retired_n++;
}

static klj_object *klj_alloc_object_locked(const char *cls, void *data) {
    klj_object *o = NULL;
    if (g_retired_n > KLJ_QUARANTINE) {
        // Recycle only a verifiably dead slot: skip anything still showing a
        // live magic. The skip is defence in depth — the quarantine should
        // make live entries impossible — but a wrong answer here is a guest
        // corruption, and a dropped entry is only a leaked slot.
        while (g_retired_n > KLJ_QUARANTINE) {
            o = &g_objects[g_retired[g_retired_head]];
            g_retired_head = (g_retired_head + 1) % KLJ_MAX_OBJECTS;
            g_retired_n--;
            if (o->magic != KLJ_OBJ_MAGIC) break;      // dead: reusable
            fprintf(stderr, "  [jni] recycle skipped live slot %u (%s)\n",
                    (unsigned)(o - g_objects), o->cls);
            o = NULL;
        }
    }
    if (!o) {
        if (g_nobjects == KLJ_MAX_OBJECTS) { KLJ_LOG("object pool exhausted"); kl_fatal_prepare(); abort(); }
        o = &g_objects[g_nobjects++];
    } else if (strcmp(o->cls, KLJ_CLASS_STRING) == 0) {
        // The old payload dies here, after the quarantine, not at retire.
        free(o->data);
    }
    *o = (klj_object){KLJ_OBJ_MAGIC, 0, cls, data};
    g_stat_alloc++;
    klj_frame_record(o);
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
    if (g_nclasses == KLJ_MAX_CLASSES) { KLJ_LOG("class table full at %s", name); kl_fatal_prepare(); abort(); }
    klj_class *c = &g_classes[g_nclasses++];
    snprintf(c->name, sizeof c->name, "%s", name);
    // A jclass is a jobject in JNI, so it gets the same representation as one.
    // Pinned: the interned class must outlive any local scope it passes through.
    c->as_object = klj_alloc_object_locked(KLJ_CLASS_CLASS, c);
    c->as_object->pinned = 1;
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


// Same as kl_jni_new_object, but carries a payload. Used for the object
// kinds the host has to *construct* rather than merely hand back: JNIBridge
// proxies, android/os/Message, and the java.lang.reflect.Method/Field objects
// that describe what to call. NOT pinned here — which lifetimes outlive a
// local frame is a per-call-site decision (proxies and queued messages are
// pinned there; a per-invoke Method is meant to die at the pop).
static void *klj_new_object_data(const char *class_name, void *data) {
    pthread_once(&g_init_once, klj_init);
    pthread_mutex_lock(&g_lock);
    klj_class  *c = klj_intern_class_locked(class_name);
    klj_object *o = klj_alloc_object_locked(c->name, data);
    pthread_mutex_unlock(&g_lock);
    return o;
}

static void *klj_class_object(const char *class_name) {
    pthread_once(&g_init_once, klj_init);
    pthread_mutex_lock(&g_lock);
    klj_class *c = klj_intern_class_locked(class_name);
    void *o = c->as_object;
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
    fprintf(out, "  objects: %llu allocated, %llu retired by DeleteLocalRef, "
                "%llu by PopLocalFrame (%u slots in use, %u quarantined)\n",
                (unsigned long long)g_stat_alloc,
                (unsigned long long)g_stat_delete,
                (unsigned long long)g_stat_pop_freed,
                g_nobjects - g_retired_n, g_retired_n);
    // What leaks: live objects by class, most common first. Built for the
    // per-frame leak hunt (swap-43582 pool exhaustion).
    {
        unsigned *cnt = calloc(g_nclasses + 2, sizeof *cnt);
        const char **names = malloc((g_nclasses + 2) * sizeof *names);
        for (unsigned c = 0; c < g_nclasses; c++) names[c] = g_classes[c].name;
        names[g_nclasses] = KLJ_CLASS_STRING;
        names[g_nclasses + 1] = KLJ_CLASS_CLASS;
        for (unsigned i = 0; i < g_nobjects; i++) {
            klj_object *o = &g_objects[i];
            if (o->magic != KLJ_OBJ_MAGIC) continue;
            for (unsigned c = 0; c < g_nclasses + 2; c++)
                if (o->cls == names[c]) { cnt[c]++; break; }
        }
        for (unsigned shown = 0; shown < 12; shown++) {
            unsigned best = 0, bn = 0;
            for (unsigned c = 0; c < g_nclasses + 2; c++)
                if (cnt[c] > bn) { bn = cnt[c]; best = c; }
            if (!bn) break;
            fprintf(out, "    live %-50s x%u\n", names[best], bn);
            cnt[best] = 0;
        }
        free(names);
        free(cnt);
    }
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
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
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
    kl_egl_report(stderr);
    kl_fatal_prepare(); abort();
}

static kl_jint klj_PushLocalFrame(void *env, kl_jint cap) {
    (void)env; (void)cap;
    klj_frame *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->parent = t_frame;
    t_frame = f;
    return 0;
}
static void *klj_PopLocalFrame(void *env, void *result) {
    (void)env;
    klj_frame *f = t_frame;
    if (!f) return result;                  // pop without push: nothing to free
    t_frame = f->parent;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < f->n; i++) {
        if (!f->objs[i] || f->objs[i] == result) continue;
        if (klj_as_object(f->objs[i])) g_stat_pop_freed++;
        klj_retire_object_locked(f->objs[i]);
    }
    // The result survives the frame; JNI promotes it to the parent frame.
    if (klj_as_object(result) && t_frame) klj_frame_record(result);
    pthread_mutex_unlock(&g_lock);
    free(f->objs);
    free(f);
    return result;
}
static kl_jint klj_EnsureLocalCapacity(void *env, kl_jint c) { (void)env; (void)c; return 0; }

// The host side of the frame pair (see kl_jni.h): playing the JVM's pop on
// native return.
void kl_jni_local_frame_push(void) { klj_PushLocalFrame(NULL, 0); }
void kl_jni_local_frame_pop(void)  { klj_PopLocalFrame(NULL, NULL); }

static void *klj_ref_identity(void *env, void *obj)       { (void)env; return obj; }
static void  klj_DeleteLocalRef(void *env, void *obj) {
    (void)env;
    if (!obj) return;                       // DeleteLocalRef(NULL) is legal
    pthread_mutex_lock(&g_lock);
    if (klj_as_object(obj)) g_stat_delete++;
    klj_retire_object_locked(obj);
    pthread_mutex_unlock(&g_lock);
}
static void *klj_NewGlobalRef(void *env, void *obj) {
    (void)env;
    klj_object *o = klj_as_object(obj);
    if (o) o->pinned++;
    return obj;
}
static void  klj_DeleteGlobalRef(void *env, void *obj) {
    (void)env;
    klj_object *o = klj_as_object(obj);
    if (o && o->pinned) o->pinned--;
}
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

// GetStringChars is the other direction, and unlike GetStringUTFChars it cannot
// be the identity: the guest wants UTF-16 and we store UTF-8. So this one really
// does copy, isCopy is true, and Release actually frees — the pair has to be
// implemented together or it silently leaks a buffer per call.
static uint16_t *klj_utf8_to_utf16(const char *s, kl_jint *out_units) {
    size_t bytes = s ? strlen(s) : 0;
    uint16_t *buf = malloc((bytes + 1) * sizeof(uint16_t));   // >= units + NUL
    if (!buf) return NULL;
    kl_jint units = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ) {
        uint32_t c;
        if (*p < 0x80)        { c = *p; p += 1; }
        else if (*p < 0xE0)   { c = (uint32_t)(*p & 0x1F) << 6  | (p[1] & 0x3F); p += 2; }
        else if (*p < 0xF0)   { c = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6
                                  | (p[2] & 0x3F); p += 3; }
        else                  { c = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12
                                  | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F); p += 4; }
        if (c >= 0x10000) {                       // split into a surrogate pair
            c -= 0x10000;
            buf[units++] = (uint16_t)(0xD800 + (c >> 10));
            buf[units++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            buf[units++] = (uint16_t)c;
        }
    }
    buf[units] = 0;
    if (out_units) *out_units = units;
    return buf;
}

static const uint16_t *klj_GetStringChars(void *env, void *jstr, uint8_t *isCopy) {
    (void)env;
    if (isCopy) *isCopy = 1;            // it is a copy, and Release must free it
    return klj_utf8_to_utf16(klj_str(jstr), NULL);
}

static void klj_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
    (void)env; (void)jstr;
    free((void *)chars);
}

// NewString takes UTF-16, which is the one direction our representation does not
// get for free. Converted rather than truncated: a cast to bytes would work for
// ASCII and silently corrupt anything else, and this is on the path Unity uses for
// text it got back from Java.
//
// Surrogate pairs are recombined into a single code point (proper UTF-8) rather
// than encoded separately (which would be modified UTF-8/CESU-8). Everything that
// reads these buffers on our side is a Darwin libc function expecting real UTF-8,
// so this is the encoding that stays consistent with the rest of the shim.
static void *klj_NewString(void *env, const uint16_t *unicode, kl_jint len) {
    (void)env;
    if (!unicode || len < 0) return NULL;
    // Worst case 3 bytes per unit; a surrogate pair is 2 units -> 4 bytes, so the
    // per-unit bound still holds.
    char *out = malloc((size_t)len * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (kl_jint i = 0; i < len; i++) {
        uint32_t c = unicode[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len &&
            unicode[i + 1] >= 0xDC00 && unicode[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (unicode[++i] - 0xDC00);
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    void *s = kl_jni_new_string(out);
    free(out);
    return s;
}

static kl_jint klj_RegisterNatives(void *env, void *clazz, const kl_jni_method *m, kl_jint n) {
    (void)env;
    const char *cls = klj_class_name(clazz);
    pthread_mutex_lock(&g_lock);
    for (kl_jint i = 0; i < n; i++) {
        if (g_nnatives == KLJ_MAX_NATIVES) { KLJ_LOG("native table full"); kl_fatal_prepare(); abort(); }
        // Copy, do not alias. The JNINativeMethod array and its strings belong to
        // the caller and may be temporary — Unity builds JNIBridge's entries
        // dynamically, and aliasing them left dangling pointers that printed as
        // "JNIBridge.d?(?" and would have made lookups match the wrong native.
        g_natives[g_nnatives++] = (klj_native){
            cls, strdup(m[i].name), strdup(m[i].signature), m[i].fnPtr};
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
    if (g_nwanted == KLJ_MAX_WANTED) { KLJ_LOG("id table full"); kl_fatal_prepare(); abort(); }
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


// FromReflectedMethod: the guest's JNIBridge.invoke hands back the very
// java.lang.reflect.Method we synthesised and asks for its jmethodID. Since a
// jmethodID here *is* the interned (class, name, signature) record, this is just
// klj_want on the description the Method object was built from — the round trip
// closes exactly because both directions agree on what identity means.
static void *klj_FromReflectedMethod(void *env, void *method) {
    (void)env;
    klj_object *o = klj_as_object(method);
    if (!o || strcmp(o->cls, KLJ_CLASS_METHOD) != 0) {
        KLJ_LOG("FromReflectedMethod: %s is not a Method", o ? o->cls : "(not an object)");
        return NULL;
    }
    klj_method_obj *m = o->data;
    if (!m) return NULL;
    return klj_want(klj_class_object(m->cls), m->name, m->sig, m->is_static ? 'M' : 'm');
}

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

// The `A` calling convention: arguments arrive as an array of jvalue unions
// rather than as a va_list. Each jvalue is 8 bytes, but a narrow type only writes
// its own width into it — so the member has to be read at the *declared* type.
// Reading .j for a jint would pick up whatever four bytes the caller happened to
// leave above it, which is a garbage argument that looks like a plausible one.
typedef union {
    uint8_t  z;  int8_t  b;  uint16_t c;  int16_t s;
    kl_jint  i;  int64_t j;  float    f;  double  d;  void *l;
} klj_jvalue;

static int klj_decode_args_a(const char *sig, const klj_jvalue *args, klj_val *argv) {
    if (!sig || *sig != '(') return -1;
    int argc = 0;
    for (const char *p = sig + 1; *p && *p != ')'; ) {
        // A NULL array is legitimate for a method that takes nothing — the guest
        // has no arguments to point at — so it is only an error once there is
        // something to read. Rejecting it outright turned every zero-argument
        // A-call into "cannot decode signature".
        if (argc == KLJ_MAX_ARGS || !args) return -1;
        while (*p == '[') p++;                       // arrays are references
        switch (*p) {
        case 'L':
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            argv[argc].l = args[argc].l;
            argc++;
            break;
        // Widened the way Java widens them: signed types sign-extend, boolean and
        // char zero-extend, so a binding reading .j sees the value it expects.
        case 'Z': argv[argc].j = (uint64_t)args[argc].z; argc++; p++; break;
        case 'B': argv[argc].j = (uint64_t)(int64_t)args[argc].b; argc++; p++; break;
        case 'C': argv[argc].j = (uint64_t)args[argc].c; argc++; p++; break;
        case 'S': argv[argc].j = (uint64_t)(int64_t)args[argc].s; argc++; p++; break;
        case 'I': argv[argc].j = (uint64_t)(int64_t)args[argc].i; argc++; p++; break;
        case 'J': argv[argc].j = (uint64_t)args[argc].j; argc++; p++; break;
        case 'F': argv[argc].d = (double)args[argc].f; argc++; p++; break;
        case 'D': argv[argc].d = args[argc].d; argc++; p++; break;
        default:  return -1;
        }
    }
    return argc;
}

// Bindings are declared against the class that *declares* the method, and the
// guest looks methods up on the class it actually has. Those differ for anything
// inherited — Unity calls getClass() on UnityPlayerActivity, and that is declared
// on java.lang.Object — so resolution walks the same superclass chain
// IsInstanceOf uses, then falls back to Object. Without this, every inherited
// method would need re-declaring against every subclass that calls it, and the
// duplicates would be the thing that drifts.
static const klj_binding *klj_find_binding(const char *cls, const char *name,
                                           const char *sig) {
    for (const klj_binding *b = g_bindings; b->cls; b++)
        if (strcmp(b->cls, cls) == 0 && strcmp(b->name, name) == 0 &&
            strcmp(b->sig, sig) == 0)
            return b;
    return NULL;
}

static const klj_binding *klj_resolve_binding(const char *cls, const char *name,
                                              const char *sig) {
    for (const char *cur = cls; cur; ) {
        const klj_binding *b = klj_find_binding(cur, name, sig);
        if (b) return b;
        const char *next = NULL;
        for (int i = 0; g_supers[i].cls; i++)
            if (strcmp(g_supers[i].cls, cur) == 0) { next = g_supers[i].super; break; }
        cur = next;
    }
    return klj_find_binding("java/lang/Object", name, sig);
}

// Exactly one of `va` / `args` is non-NULL; everything else about a call is
// identical between the two conventions, so they share this path rather than
// duplicating the binding lookup and the diagnostics.
static klj_val klj_call_common(void *env, void *self, void *mid, char want,
                               kl_va *va, const klj_jvalue *args) {
    klj_val zero = {0};
    klj_wanted *w = mid;
    if (!w || w < g_wanted || w >= g_wanted + KLJ_MAX_WANTED) {
        KLJ_LOG("Call*Method with a jmethodID we never issued (%p)", mid);
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }

    char have = klj_return_kind(w->sig);
    if (want != '?' && have != want)
        KLJ_LOG("WARNING %s.%s%s called through a '%c' slot but returns '%c'",
                w->cls, w->name, w->sig, want, have);

    const klj_binding *b = klj_resolve_binding(w->cls, w->name, w->sig);
    if (b) {
        klj_val argv[KLJ_MAX_ARGS];
        int argc = va ? klj_decode_args(w->sig, va, argv)
                      : klj_decode_args_a(w->sig, args, argv);
        if (argc < 0) {
            KLJ_LOG("cannot decode signature %s for %s.%s", w->sig, w->cls, w->name);
            kl_fatal_prepare(); abort();
        }
        return b->fn(env, self, argv, argc);
    }

    KLJ_LOG("no host implementation for %s.%s%s", w->cls, w->name, w->sig);
    if (!g_permissive) {
        fprintf(stderr, "[jni] this is an M4 work item — add it to g_bindings.\n");
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    return zero;
}

static klj_val klj_call(void *env, void *self, void *mid, kl_va *va, char want) {
    return klj_call_common(env, self, mid, want, va, NULL);
}
static klj_val klj_call_a(void *env, void *self, void *mid,
                          const klj_jvalue *args, char want) {
    return klj_call_common(env, self, mid, want, NULL, args);
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

// The same family over the `A` convention. Generated rather than typed out for
// the reason the V forms are: these differ only in the return kind, and a
// hand-written set is where a copy-paste picks the wrong one. Nonvirtual is the
// instance form as far as we are concerned — there is no class hierarchy here to
// dispatch through, so "call exactly this method" is what every form already does.
#define KLJ_CALL_A(Name, Ret, Kind, Expr)                                                 \
    static Ret klj_Call##Name##MethodA(void *env, void *self, void *mid, const klj_jvalue *a) { \
        klj_val r = klj_call_a(env, self, mid, a, Kind); (void)r; return Expr;              \
    }                                                                                       \
    static Ret klj_CallStatic##Name##MethodA(void *env, void *cls, void *mid, const klj_jvalue *a) { \
        klj_val r = klj_call_a(env, cls, mid, a, Kind); (void)r; return Expr;                \
    }                                                                                       \
    static Ret klj_CallNonvirtual##Name##MethodA(void *env, void *self, void *cls,          \
                                                 void *mid, const klj_jvalue *a) {          \
        (void)cls;                                                                          \
        klj_val r = klj_call_a(env, self, mid, a, Kind); (void)r; return Expr;               \
    }
KLJ_CALL_A(Object,  void *,   'L', r.l)
KLJ_CALL_A(Boolean, uint8_t,  'Z', (uint8_t)r.j)
KLJ_CALL_A(Byte,    int8_t,   'B', (int8_t)r.j)
KLJ_CALL_A(Char,    uint16_t, 'C', (uint16_t)r.j)
KLJ_CALL_A(Short,   int16_t,  'S', (int16_t)r.j)
KLJ_CALL_A(Int,     kl_jint,  'I', (kl_jint)r.j)
KLJ_CALL_A(Long,    int64_t,  'J', (int64_t)r.j)
KLJ_CALL_A(Float,   float,    'F', (float)r.d)
KLJ_CALL_A(Double,  double,   'D', r.d)
#undef KLJ_CALL_A
static void klj_CallVoidMethodA(void *env, void *self, void *mid, const klj_jvalue *a) {
    klj_call_a(env, self, mid, a, 'V');
}
static void klj_CallStaticVoidMethodA(void *env, void *cls, void *mid, const klj_jvalue *a) {
    klj_call_a(env, cls, mid, a, 'V');
}
static void klj_CallNonvirtualVoidMethodA(void *env, void *self, void *cls, void *mid,
                                          const klj_jvalue *a) {
    (void)cls;
    klj_call_a(env, self, mid, a, 'V');
}
static void *klj_NewObjectA(void *env, void *clazz, void *mid, const klj_jvalue *a) {
    return klj_call_a(env, clazz, mid, a, '?').l;
}

// A constructor is just a method named <init>; it returns the new object rather
// than the 'V' its signature claims, so the slot check is skipped.
static void *klj_NewObjectV(void *env, void *clazz, void *mid, kl_va *va) {
    return klj_call(env, clazz, mid, va, '?').l;
}

// ----------------------------------------------------------------- Java arrays
// One representation for both flavours: object arrays hold jobjects, primitive
// arrays hold a raw buffer. `kind` is the JNI type character, so the array's
// class name is "[" + kind (or "[L<elem>;"), which is exactly what the guest
// would see from GetObjectClass.
typedef struct {
    int    len;
    char   kind;    // 'L' object, else 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D'
    size_t elem;    // element size in bytes (primitive arrays)
    void  *data;    // void** when kind=='L', else a raw buffer
} klj_array;

static size_t klj_prim_size(char kind) {
    switch (kind) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default:            return 0;
    }
}

static void *klj_new_array(char kind, const char *elem_cls, int len) {
    if (len < 0) len = 0;
    klj_array *arr = calloc(1, sizeof *arr);
    arr->len  = len;
    arr->kind = kind;
    if (kind == 'L') {
        arr->data = calloc((size_t)len, sizeof(void *));
    } else {
        arr->elem = klj_prim_size(kind);
        arr->data = calloc((size_t)len, arr->elem ? arr->elem : 1);
    }
    char cls[256];
    if (kind == 'L') snprintf(cls, sizeof cls, "[L%s;", elem_cls ? elem_cls : "java/lang/Object");
    else             snprintf(cls, sizeof cls, "[%c", kind);
    void *obj = kl_jni_new_object(cls);
    klj_as_object(obj)->data = arr;
    return obj;
}

static klj_array *klj_arr(void *a) {
    klj_object *o = klj_as_object(a);
    return o ? o->data : NULL;
}

static kl_jint klj_GetArrayLength(void *env, void *a) {
    (void)env;
    klj_array *arr = klj_arr(a);
    return arr ? arr->len : 0;
}

static void *klj_NewObjectArray(void *env, kl_jint len, void *elemClass, void *init) {
    (void)env;
    void      *obj = klj_new_array('L', klj_class_name(elemClass), len);
    klj_array *arr = klj_arr(obj);
    if (init)
        for (int i = 0; i < arr->len; i++) ((void **)arr->data)[i] = init;
    return obj;
}
static void *klj_GetObjectArrayElement(void *env, void *a, kl_jint i) {
    (void)env;
    klj_array *arr = klj_arr(a);
    if (!arr || arr->kind != 'L' || i < 0 || i >= arr->len) return NULL;
    return ((void **)arr->data)[i];
}
static void klj_SetObjectArrayElement(void *env, void *a, kl_jint i, void *v) {
    (void)env;
    klj_array *arr = klj_arr(a);
    if (!arr || arr->kind != 'L' || i < 0 || i >= arr->len) return;
    ((void **)arr->data)[i] = v;
}

// Primitive arrays. New<T>Array / Get<T>ArrayElements / Release / Get-SetRegion
// all reduce to the same four shapes, so they are generated rather than typed
// out nine times — the same reasoning as the JNI slot table.
#define KLJ_PRIM_ARRAY(Name, kind)                                                    \
    static void *klj_New##Name##Array(void *env, kl_jint len) {                       \
        (void)env; return klj_new_array(kind, NULL, len);                             \
    }                                                                                 \
    static void *klj_Get##Name##ArrayElements(void *env, void *a, uint8_t *isCopy) {  \
        (void)env; if (isCopy) *isCopy = 0;                                           \
        klj_array *arr = klj_arr(a); return arr ? arr->data : NULL;                   \
    }                                                                                 \
    static void klj_Release##Name##ArrayElements(void *env, void *a, void *p, kl_jint m) { \
        (void)env; (void)a; (void)p; (void)m;   /* never a copy, nothing to write back */ \
    }                                                                                 \
    static void klj_Get##Name##ArrayRegion(void *env, void *a, kl_jint start,         \
                                           kl_jint len, void *buf) {                  \
        (void)env; klj_array *arr = klj_arr(a);                                       \
        if (!arr || start < 0 || len < 0 || start + len > arr->len) return;           \
        memcpy(buf, (char *)arr->data + (size_t)start * arr->elem, (size_t)len * arr->elem); \
    }                                                                                 \
    static void klj_Set##Name##ArrayRegion(void *env, void *a, kl_jint start,         \
                                           kl_jint len, const void *buf) {            \
        (void)env; klj_array *arr = klj_arr(a);                                       \
        if (!arr || start < 0 || len < 0 || start + len > arr->len) return;           \
        memcpy((char *)arr->data + (size_t)start * arr->elem, buf, (size_t)len * arr->elem); \
    }
KLJ_PRIM_ARRAY(Boolean, 'Z') KLJ_PRIM_ARRAY(Byte,  'B') KLJ_PRIM_ARRAY(Char,   'C')
KLJ_PRIM_ARRAY(Short,   'S') KLJ_PRIM_ARRAY(Int,   'I') KLJ_PRIM_ARRAY(Long,   'J')
KLJ_PRIM_ARRAY(Float,   'F') KLJ_PRIM_ARRAY(Double,'D')
#undef KLJ_PRIM_ARRAY

// -------------------------------------------------------- Java field dispatch
// Fields are simpler than methods: everything the guest reads so far is a
// compile-time constant off a framework class, so the binding is a value rather
// than a function. `cached` keeps a constant's jstring identity stable across
// reads, which is what a real static final String would do.
typedef klj_val (*klj_field_fn)(void);   // for values only known at runtime

typedef struct {
    const char  *cls, *name, *sig;
    const char  *sval;    // for object fields — String constants
    int64_t      ival;    // for integral primitive fields
    double       dval;    // for float/double fields — a separate slot, because
                          // klj_val is a union and the caller's `want` decides
                          // which half of it is read
    klj_field_fn fn;      // takes precedence: paths and other configured values
} klj_field;
static const klj_field g_fields[];

// Interned jstrings for constant object fields, parallel to g_fields so the
// table itself stays const. A static final String read twice is the same object
// in Java, and some callers do compare identity.
#define KLJ_MAX_FIELDS 128
static void *g_field_cache[KLJ_MAX_FIELDS];

// Defined further down with the rest of the Java implementations.
static void   *klj_new_file(const char *path);
static klj_val klj_appinfo_sourceDir(void);
static klj_val klj_appinfo_nativeLibraryDir(void);
static klj_val klj_appinfo_dataDir(void);
static klj_val klj_appinfo_splitSourceDirs(void);
static klj_val klj_metaData_field(void);
static klj_val klj_currentActivity_field(void);

// ---- written fields ----
// g_fields describes the device, and a description does not change. But some
// objects we hand out are genuinely mutable: Java's idiom for window attributes
// is to fetch the live LayoutParams, assign to a field and hand it back, so a
// write that went nowhere would read back as the default and silently undo the
// caller's intent. Writes are therefore recorded per (object, field) and take
// precedence on read — which also makes a field the guest writes before it ever
// reads legal, without it having to be declared in g_fields at all.
//
// Statics work through the same table: the "object" is then the jclass, which is
// interned and so just as stable an identity.
#define KLJ_MAX_FIELD_WRITES 128
static struct { void *obj, *fid; klj_val v; } g_field_writes[KLJ_MAX_FIELD_WRITES];
static unsigned g_nfield_writes;

static klj_val *klj_find_write(void *obj, void *fid) {
    for (unsigned i = 0; i < g_nfield_writes; i++)
        if (g_field_writes[i].obj == obj && g_field_writes[i].fid == fid)
            return &g_field_writes[i].v;
    return NULL;
}

static void klj_field_store(void *obj, void *fid, klj_val v) {
    klj_wanted *w = fid;
    if (!w || w < g_wanted || w >= g_wanted + KLJ_MAX_WANTED) {
        KLJ_LOG("Set*Field with a jfieldID we never issued (%p)", fid);
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    klj_val *slot = klj_find_write(obj, fid);
    if (!slot) {
        if (g_nfield_writes == KLJ_MAX_FIELD_WRITES) {
            KLJ_LOG("field write table full at %s.%s", w->cls, w->name);
            kl_fatal_prepare(); abort();
        }
        g_field_writes[g_nfield_writes] = (typeof(g_field_writes[0])){obj, fid, v};
        slot = &g_field_writes[g_nfield_writes++].v;
        KLJ_LOG("Set %s.%s%s = 0x%llx", w->cls, w->name, w->sig, (unsigned long long)v.j);
    }
    *slot = v;
}

static void klj_SetObjectField(void *e, void *o, void *f, void *v)   { (void)e; klj_field_store(o, f, (klj_val){.l = v}); }
static void klj_SetIntField(void *e, void *o, void *f, kl_jint v)    { (void)e; klj_field_store(o, f, (klj_val){.j = (uint64_t)(int64_t)v}); }
static void klj_SetLongField(void *e, void *o, void *f, int64_t v)   { (void)e; klj_field_store(o, f, (klj_val){.j = (uint64_t)v}); }
static void klj_SetBooleanField(void *e, void *o, void *f, uint8_t v){ (void)e; klj_field_store(o, f, (klj_val){.j = v}); }
static void klj_SetFloatField(void *e, void *o, void *f, float v)    { (void)e; klj_field_store(o, f, (klj_val){.d = v}); }

static klj_val klj_field_value(void *obj, void *fid, char want) {
    klj_wanted *w = fid;
    if (!w || w < g_wanted || w >= g_wanted + KLJ_MAX_WANTED) {
        KLJ_LOG("Get*Field with a jfieldID we never issued (%p)", fid);
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    const klj_val *written = klj_find_write(obj, fid);
    if (written) return *written;
    for (const klj_field *f = g_fields; f->cls; f++) {
        if (strcmp(f->cls, w->cls) || strcmp(f->name, w->name) || strcmp(f->sig, w->sig))
            continue;
        if (f->fn) return f->fn();
        if (want == 'L') {
            size_t idx = (size_t)(f - g_fields);
            if (idx < KLJ_MAX_FIELDS) {
                if (!g_field_cache[idx]) g_field_cache[idx] = kl_jni_new_string(f->sval);
                return (klj_val){.l = g_field_cache[idx]};
            }
            return (klj_val){.l = kl_jni_new_string(f->sval)};
        }
        if (want == 'F' || want == 'D') return (klj_val){.d = f->dval};
        return (klj_val){.j = (uint64_t)f->ival};
    }
    KLJ_LOG("no host value for field %s.%s %s", w->cls, w->name, w->sig);
    if (!g_permissive) {
        fprintf(stderr, "[jni] this is an M4 work item — add it to g_fields.\n");
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    return (klj_val){0};
}

// Static and instance forms run the same lookup — the only difference is that
// the "object" is the jclass, which the write table keys on just as well.
static void   *klj_GetStaticObjectField(void *e, void *c, void *f) { (void)e; return klj_field_value(c, f, 'L').l; }
static void   *klj_GetObjectField(void *e, void *o, void *f)       { (void)e; return klj_field_value(o, f, 'L').l; }
static kl_jint klj_GetStaticIntField(void *e, void *c, void *f)    { (void)e; return (kl_jint)klj_field_value(c, f, 'I').j; }
static kl_jint klj_GetIntField(void *e, void *o, void *f)          { (void)e; return (kl_jint)klj_field_value(o, f, 'I').j; }
static uint8_t klj_GetStaticBooleanField(void *e, void *c, void *f){ (void)e; return (uint8_t)klj_field_value(c, f, 'Z').j; }
static uint8_t klj_GetBooleanField(void *e, void *o, void *f)      { (void)e; return (uint8_t)klj_field_value(o, f, 'Z').j; }
static float   klj_GetStaticFloatField(void *e, void *c, void *f)  { (void)e; return (float)klj_field_value(c, f, 'F').d; }
static float   klj_GetFloatField(void *e, void *o, void *f)        { (void)e; return (float)klj_field_value(o, f, 'F').d; }

// Documented Android platform constants — fixed values, not choices.
#define KLJ_FSTR(c, n, v)     {.cls = c, .name = n, .sig = "Ljava/lang/String;", .sval = v}
#define KLJ_FINT(c, n, v)     {.cls = c, .name = n, .sig = "I", .ival = v}
#define KLJ_FFLT(c, n, v)     {.cls = c, .name = n, .sig = "F", .dval = v}
#define KLJ_FFN(c, n, s, f)   {.cls = c, .name = n, .sig = s, .fn = f}
#define KLJ_CTX_SVC(field, name) KLJ_FSTR("android/content/Context", field, name)

// ---- the presented display ----
// One description of the screen, read by everything that asks about it: the
// Display, the DisplayMetrics fields, and Resources. Deciding it in one place is
// the point — Unity derives its render target size, its UI scale and its frame
// pacing from these numbers, and answering each call on its own terms would let
// them disagree with each other and with the ANativeWindow in kl_ndk.c.
//
// The geometry is a Quest 2's per-eye panel, matching that window, and for the
// same reason Build.MODEL is a Quest 2's below: this title branches on Oculus
// hardware. It is a placeholder in the same sense kl_ndk.c's is — in VR the eye
// buffers come from the XR runtime (M6) rather than from the Android display,
// and this exists to give Unity a coherent non-zero screen at startup. 72 Hz is
// the Quest 2's default mode; 90 is opt-in, and claiming it would have Unity
// pace to a rate M5 cannot yet deliver. Density is a choice rather than a
// measurement — it only scales 2D UI — and xhdpi suits a panel this fine.
//
// Macros rather than a struct because g_fields is a static initialiser and a
// const object is not a constant expression in C.
#define KLJ_DISPLAY_W        1832
#define KLJ_DISPLAY_H        1920
#define KLJ_DISPLAY_DPI      320
#define KLJ_DISPLAY_DENSITY  2.0     /* xhdpi: densityDpi / 160 */
#define KLJ_DISPLAY_XDPI     320.0
#define KLJ_DISPLAY_YDPI     320.0
#define KLJ_DISPLAY_REFRESH  72.0f
#define KLJ_DISPLAY_ROTATION 0       /* Surface.ROTATION_0 */

static const klj_field g_fields[] = {
    // Audio. The PROPERTY_* values are the real Android key strings, so the
    // getProperty implementation can match on them rather than on our own names.
    KLJ_FSTR("android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE",
             "android.media.property.OUTPUT_SAMPLE_RATE"),
    KLJ_FSTR("android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER",
             "android.media.property.OUTPUT_FRAMES_PER_BUFFER"),
    KLJ_FINT("android/media/AudioManager", "GET_DEVICES_OUTPUTS", 2),
    KLJ_FINT("android/media/AudioManager", "STREAM_MUSIC", 3),

    // View.SYSTEM_UI_FLAG_* and the window flag behind them. Real Android
    // values: Unity ORs these together and hands the result straight back to
    // setSystemUiVisibility, so a wrong bit here is a wrong request there.
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_FULLSCREEN",            0x00000004),
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_HIDE_NAVIGATION",       0x00000002),
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_LAYOUT_STABLE",         0x00000100),
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION",0x00000200),
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN",     0x00000400),
    KLJ_FINT("android/view/View", "SYSTEM_UI_FLAG_IMMERSIVE_STICKY",      0x00001000),
    KLJ_FINT("android/view/WindowManager$LayoutParams", "FLAG_KEEP_SCREEN_ON", 0x00000080),
    KLJ_FSTR("android/content/pm/PackageManager", "FEATURE_AUDIO_LOW_LATENCY",
             "android.hardware.audio.low_latency"),
    KLJ_FINT("android/content/pm/PackageManager", "PERMISSION_GRANTED", 0),

    KLJ_FSTR("android/content/Intent", "ACTION_MAIN", "android.intent.action.MAIN"),

    KLJ_FSTR("android/os/Environment", "MEDIA_MOUNTED", "mounted"),

    // ApplicationInfo is read field-by-field, and these depend on runtime
    // configuration rather than being compile-time constants.
    KLJ_FFN("android/content/pm/ApplicationInfo", "sourceDir",        "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "publicSourceDir",  "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "nativeLibraryDir", "Ljava/lang/String;", klj_appinfo_nativeLibraryDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "dataDir",          "Ljava/lang/String;", klj_appinfo_dataDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitSourceDirs",       "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitPublicSourceDirs", "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FINT("android/content/pm/ApplicationInfo", "flags", 0),

    KLJ_FSTR("android/content/pm/PackageInfo", "versionName", "1.28.0_4124311467"),
    KLJ_FINT("android/content/pm/PackageInfo", "versionCode", 545),
    KLJ_FSTR("android/content/pm/PackageInfo", "packageName", "com.beatgames.beatsaber"),

    // Unity's own static handle on the Activity. Must be the *same* object the
    // Context was, not another instance of the class — Unity passes one to native
    // code and reads the other back, then compares them.
    KLJ_FFN("com/unity3d/player/UnityPlayer", "currentActivity", "Ljava/lang/Object;", klj_currentActivity_field),

    KLJ_FFN("android/content/pm/PackageItemInfo", "metaData", "Landroid/os/Bundle;", klj_metaData_field),
    KLJ_FFN("android/content/pm/ApplicationInfo", "metaData", "Landroid/os/Bundle;", klj_metaData_field),

    // We present a Quest-shaped device, and that is a deliberate choice rather
    // than an oversight. The guest's whole VR stack branches on these: Oculus
    // code checks MANUFACTURER, and AndroidManifest.xml declares
    // com.oculus.supportedDevices="quest|delmar" (delmar is Quest 2). Reporting
    // Apple hardware would be more literally true and would fail every one of
    // those checks. API 29 is what the Quest 2 reported for this build's era —
    // below the app's targetSdkVersion of 30, so Unity takes the compatibility
    // paths the title actually shipped against. Revisit if a path needs 30+.
    KLJ_FINT("android/os/Build$VERSION", "SDK_INT", 29),
    KLJ_FSTR("android/os/Build$VERSION", "RELEASE", "10"),
    // The build-number half of the version. Matches Build.ID so the two halves of
    // the fingerprint describe one build.
    KLJ_FSTR("android/os/Build$VERSION", "INCREMENTAL", "SQ3A.220605.009.A1"),
    KLJ_FSTR("android/os/Build$VERSION", "CODENAME",    "REL"),
    KLJ_FSTR("android/os/Build", "MANUFACTURER", "Oculus"),
    KLJ_FSTR("android/os/Build", "BRAND",        "oculus"),
    KLJ_FSTR("android/os/Build", "MODEL",        "Quest 2"),
    KLJ_FSTR("android/os/Build", "DEVICE",       "delmar"),
    KLJ_FSTR("android/os/Build", "PRODUCT",      "delmar"),
    KLJ_FSTR("android/os/Build", "HARDWARE",     "qcom"),
    // Build fingerprint pieces, in the shape Android defines and consistent with
    // the Quest 2 described above. Unity puts these straight into its device
    // report; nothing branches on them, but they have to parse and to agree with
    // MODEL/DEVICE rather than describing some other machine.
    KLJ_FSTR("android/os/Build", "ID",           "SQ3A.220605.009.A1"),
    KLJ_FSTR("android/os/Build", "DISPLAY",      "SQ3A.220605.009.A1"),
    KLJ_FSTR("android/os/Build", "TYPE",         "user"),
    KLJ_FSTR("android/os/Build", "TAGS",         "release-keys"),
    KLJ_FSTR("android/os/Build", "FINGERPRINT",
             "oculus/delmar/delmar:10/SQ3A.220605.009.A1/1:user/release-keys"),


    KLJ_FINT("android/content/Context", "MODE_PRIVATE", 0),

    KLJ_CTX_SVC("LOCATION_SERVICE",     "location"),
    KLJ_CTX_SVC("AUDIO_SERVICE",        "audio"),
    KLJ_CTX_SVC("WINDOW_SERVICE",       "window"),
    KLJ_CTX_SVC("ACTIVITY_SERVICE",     "activity"),
    KLJ_CTX_SVC("SENSOR_SERVICE",       "sensor"),
    KLJ_CTX_SVC("POWER_SERVICE",        "power"),
    KLJ_CTX_SVC("VIBRATOR_SERVICE",     "vibrator"),
    KLJ_CTX_SVC("CONNECTIVITY_SERVICE", "connectivity"),
    KLJ_CTX_SVC("WIFI_SERVICE",         "wifi"),
    KLJ_CTX_SVC("INPUT_SERVICE",        "input"),
    KLJ_CTX_SVC("DISPLAY_SERVICE",      "display"),
    KLJ_CTX_SVC("CLIPBOARD_SERVICE",    "clipboard"),
    KLJ_CTX_SVC("NOTIFICATION_SERVICE", "notification"),
    KLJ_CTX_SVC("MEDIA_ROUTER_SERVICE", "media_router"),

    // Unity asks the MediaRouter for the live-video route to find out whether it
    // should be presenting to an external display. Values are from android-34's
    // android.jar, not from memory.
    KLJ_FINT("android/media/MediaRouter", "ROUTE_TYPE_LIVE_AUDIO", 1),
    KLJ_FINT("android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", 2),
    KLJ_FINT("android/media/MediaRouter", "ROUTE_TYPE_USER",       8388608),

    KLJ_FSTR("android/provider/Settings$Secure", "ANDROID_ID", "android_id"),

    KLJ_FINT("android/content/pm/PackageManager", "GET_ACTIVITIES",     0x0001),
    KLJ_FINT("android/content/pm/PackageManager", "GET_INTENT_FILTERS", 0x0020),
    KLJ_FINT("android/content/pm/PackageManager", "GET_META_DATA",      0x0080),

    // The whole documented ActivityInfo orientation table. Transcribed rather
    // than trimmed to what the trace forced: the guest reads these to compare
    // against a value it is about to pass to setRequestedOrientation, so a
    // missing one would show up as a wrong branch rather than as a lookup.
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_UNSPECIFIED",       -1),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_LANDSCAPE",          0),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_PORTRAIT",           1),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER",               2),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_BEHIND",             3),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR",             4),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_NOSENSOR",           5),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE",   6),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR_PORTRAIT",    7),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_REVERSE_LANDSCAPE",  8),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_REVERSE_PORTRAIT",   9),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_FULL_SENSOR",       10),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER_LANDSCAPE",    11),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER_PORTRAIT",     12),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_FULL_USER",         13),
    KLJ_FINT("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_LOCKED",            14),

    // Cutout modes. DEFAULT means "do not extend into the cutout", which is the
    // only sensible answer for a display we describe as having none.
    KLJ_FINT("android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT",     0),
    KLJ_FINT("android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES", 1),
    KLJ_FINT("android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER",       2),

    KLJ_FINT("android/view/Display", "DEFAULT_DISPLAY", 0),

    // DisplayMetrics is read field-by-field off an instance, and our field
    // dispatch is per (class, name, signature) rather than per object — which is
    // exactly right here, because there is one display and every DisplayMetrics
    // the guest fills from it describes that same screen.
    KLJ_FINT("android/util/DisplayMetrics", "widthPixels",    KLJ_DISPLAY_W),
    KLJ_FINT("android/util/DisplayMetrics", "heightPixels",   KLJ_DISPLAY_H),
    KLJ_FINT("android/util/DisplayMetrics", "densityDpi",     KLJ_DISPLAY_DPI),
    KLJ_FFLT("android/util/DisplayMetrics", "density",        KLJ_DISPLAY_DENSITY),
    KLJ_FFLT("android/util/DisplayMetrics", "scaledDensity",  KLJ_DISPLAY_DENSITY),
    KLJ_FFLT("android/util/DisplayMetrics", "xdpi",           KLJ_DISPLAY_XDPI),
    KLJ_FFLT("android/util/DisplayMetrics", "ydpi",           KLJ_DISPLAY_YDPI),
    {.cls = NULL},
};

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
// Every path we hand the guest must be absolute. Android's are — getPackageCodePath
// returns /data/app/<pkg>/base.apk and getFilesDir /data/data/<pkg>/files — and
// Unity relies on it: it mounts the APK into its VFS under the path it was given
// and later resolves entries by concatenating onto that mount point. A relative
// mount point survives the mount and then fails to match, so the lookup falls
// through to a raw open() of "beatsaber.apk/assets/..." — which is not a
// directory, and Unity reports it as "Not enough storage space to install
// required resources."
//
// realpath() is not usable here: several of these name directories we have not
// created yet. Prefixing the cwd is enough, since that is what a relative path
// already meant.
static const char *klj_abspath(const char *p) {
    if (!p || p[0] == '/') return p;
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) return p;
    size_t n = strlen(cwd) + strlen(p) + 2;
    char  *out = malloc(n);
    snprintf(out, n, "%s/%s", cwd, p);
    return out;
}

static const char *g_assets_dir = "beatsaber/assets";
void kl_jni_set_assets_dir(const char *dir) { g_assets_dir = klj_abspath(dir); }

static klj_val klj_singleton(const char *cls, void **slot) {
    if (!*slot) {
        *slot = kl_jni_new_object(cls);
        ((klj_object *)*slot)->pinned = 1;   // host-held: survives frame pops
    }
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
// new Intent(action). The action string is the only part anything reads so far.
static klj_val klj_Intent_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    const char *action = n > 0 ? klj_str(a[0].l) : NULL;
    void       *obj    = kl_jni_new_object("android/content/Intent");
    klj_as_object(obj)->data = action ? strdup(action) : NULL;
    KLJ_LOG("new Intent(\"%s\")", action ? action : "(null)");
    return (klj_val){.l = obj};
}
// Intent's builder methods all return `this` — that is the Java contract, not a
// convenience. Nothing reads the categories back, so they are logged, not stored.
static klj_val klj_Intent_addCategory(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *c = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("Intent.addCategory(\"%s\")", c ? c : "(null)");
    return (klj_val){.l = self};
}
static klj_val klj_Intent_setPackage(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *p = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("Intent.setPackage(\"%s\")", p ? p : "(null)");
    return (klj_val){.l = self};
}
static klj_val klj_Intent_addFlags(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    KLJ_LOG("Intent.addFlags(0x%llx)", n > 0 ? (unsigned long long)a[0].j : 0ULL);
    return (klj_val){.l = self};
}
// A minimal java/util/List. Fixed contents — nothing mutates one of ours.
typedef struct { void **items; int count; } klj_list;

static void *klj_new_list(void **items, int count) {
    klj_list *l = calloc(1, sizeof *l);
    l->items = items;
    l->count = count;
    void *obj = kl_jni_new_object("java/util/List");
    klj_as_object(obj)->data = l;
    return obj;
}
static klj_val klj_List_size(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_list   *l = o ? o->data : NULL;
    return (klj_val){.j = l ? (uint64_t)l->count : 0};
}
static klj_val klj_List_isEmpty(void *env, void *self, const klj_val *a, int n) {
    return (klj_val){.j = klj_List_size(env, self, a, n).j == 0};
}
static klj_val klj_List_get(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o = klj_as_object(self);
    klj_list   *l = o ? o->data : NULL;
    int         i = n > 0 ? (int)a[0].j : -1;
    if (!l || i < 0 || i >= l->count) return (klj_val){.l = NULL};
    return (klj_val){.l = l->items[i]};
}

// Unity asks whether its *own* activity is registered under the VR category —
// ACTION_MAIN + com.oculus.intent.category.VR, setPackage(our package). Our
// AndroidManifest.xml does declare exactly that on UnityPlayerActivity, so the
// truthful answer is one match. An empty list would read as "this is not a VR
// app" and is the kind of convenient lie that disables the path under test.
static klj_val klj_PM_queryIntentActivities(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void  *ri;
    static void  *items[1];
    static void  *list;
    if (!list) {
        ri       = kl_jni_new_object("android/content/pm/ResolveInfo");
        items[0] = ri;
        list     = klj_new_list(items, 1);
    }
    return (klj_val){.l = list};
}

// Object.getClass() is GetObjectClass reached the other way round — the guest
// calls it as a Java method on classes it has no jclass for yet.
static klj_val klj_Object_getClass(void *env, void *self, const klj_val *a, int n) {
    (void)a; (void)n;
    return (klj_val){.l = klj_GetObjectClass(env, self)};
}

// A constructor whose only job is to produce an identity. `self` is the jclass
// NewObject was given, so this stays correct for any class that needs no state —
// unlike the hardcoded class name a per-type <init> would use.
static klj_val klj_generic_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object(klj_class_name(clazz))};
}

// Unity's JNIBridge builds a java.lang.reflect.Proxy implementing the given
// interfaces, backed by a native pointer; when Android later calls a method on
// it, JNIBridge.invoke(ptr, Class, Method, Object[]) forwards into native code.
// We are the Java side, so the proxy is just an object remembering the pointer.
// Nothing calls back into it until we start synthesising Android events, but the
// interface list is worth logging — it names every callback the engine expects.
typedef struct { int64_t native_ptr; void *classes; } klj_proxy;

static klj_val klj_JNIBridge_newInterfaceProxy(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_proxy *p = calloc(1, sizeof *p);
    p->native_ptr = n > 0 ? (int64_t)a[0].j : 0;
    p->classes    = n > 1 ? a[1].l : NULL;

    klj_array *ifaces = p->classes ? klj_arr(p->classes) : NULL;
    if (ifaces && ifaces->kind == 'L')
        for (int i = 0; i < ifaces->len; i++)
            KLJ_LOG("newInterfaceProxy: implements %s (native 0x%llx)",
                    klj_class_name(((void **)ifaces->data)[i]),
                    (unsigned long long)p->native_ptr);

    void *obj = kl_jni_new_object("bitter/jnibridge/JNIBridge$Proxy");
    klj_as_object(obj)->data = p;
    klj_as_object(obj)->pinned = 1;   // guest-held long-term via native_ptr
    return (klj_val){.l = obj};
}

static klj_val klj_Context_getPackageManager(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *pm;
    return klj_singleton("android/content/pm/PackageManager", &pm);
}
// The APK's real package name, from AndroidManifest.xml. Unity uses it to look
// itself up through the PackageManager and to derive storage paths.
static klj_val klj_Context_getPackageName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("com.beatgames.beatsaber")};
}
// getSystemService returns the manager object for a service name. Returning null
// for an unknown one is legitimate Android — a device need not offer every
// service — so unknowns are logged rather than fabricated.
static klj_val klj_Context_getSystemService(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static const struct { const char *svc, *cls; } services[] = {
        {"location",     "android/location/LocationManager"},
        {"audio",        "android/media/AudioManager"},
        {"window",       "android/view/WindowManager"},
        {"activity",     "android/app/ActivityManager"},
        {"sensor",       "android/hardware/SensorManager"},
        {"power",        "android/os/PowerManager"},
        {"vibrator",     "android/os/Vibrator"},
        {"connectivity", "android/net/ConnectivityManager"},
        {"wifi",         "android/net/wifi/WifiManager"},
        {"input",        "android/hardware/input/InputManager"},
        {"display",      "android/hardware/display/DisplayManager"},
        {"clipboard",    "android/content/ClipboardManager"},
        {"notification", "android/app/NotificationManager"},
        {"media_router", "android/media/MediaRouter"},
        {NULL, NULL},
    };
    const char *want = n > 0 ? klj_str(a[0].l) : NULL;
    if (!want) return (klj_val){.l = NULL};
    // One instance per service, as Android does — callers compare identity.
    static void *cache[sizeof services / sizeof services[0]];
    for (unsigned i = 0; services[i].svc; i++) {
        if (strcmp(services[i].svc, want)) continue;
        if (!cache[i]) cache[i] = kl_jni_new_object(services[i].cls);
        KLJ_LOG("getSystemService(\"%s\") -> %s", want, services[i].cls);
        return (klj_val){.l = cache[i]};
    }
    KLJ_LOG("getSystemService(\"%s\") -> null (unknown service)", want);
    return (klj_val){.l = NULL};
}

// String.equals compares content, not identity — which matters, because our
// jstrings are freshly allocated per call and a constant read twice is not the
// same object. Anything comparing strings must come through here.
static klj_val klj_String_equals(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *x = klj_str(self);
    const char *y = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = (x && y && strcmp(x, y) == 0)};
}
static klj_val klj_String_length(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *s = klj_str(self);
    return (klj_val){.j = s ? strlen(s) : 0};
}

// ---- storage ----
// Unity asks the Context where it may write: Application.persistentDataPath comes
// from getExternalFilesDir, and saves and player prefs land there. These must be
// real, writable directories — a stub path would make the first write fail deep
// inside the engine rather than here.
static const char *g_files_dir = "build/android-files";
void kl_jni_set_files_dir(const char *dir) { g_files_dir = klj_abspath(dir); }

// The APK itself. Unity opens this as a zip to read streaming assets, so it has
// to be a real file — the unpacked tree under beatsaber/ is not a substitute.
static const char *g_apk_path = "beatsaber.apk";
void kl_jni_set_apk_path(const char *path) { g_apk_path = klj_abspath(path); }

static klj_val klj_Context_getPackageCodePath(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("getPackageCodePath() -> %s", g_apk_path);
    return (klj_val){.l = kl_jni_new_string(g_apk_path)};
}

// Where the guest's own .so files live. Unity reads this to dlopen further
// libraries, and our guest dlopen resolves against the same directory.
static const char *g_native_lib_dir = "beatsaber/lib/arm64-v8a";
void kl_jni_set_native_lib_dir(const char *dir) { g_native_lib_dir = klj_abspath(dir); }

// ApplicationInfo is a plain data holder — Unity reads its fields directly
// rather than calling accessors, so these are field getters, not methods.
static klj_val klj_appinfo_sourceDir(void)     { return (klj_val){.l = kl_jni_new_string(g_apk_path)}; }
static klj_val klj_appinfo_nativeLibraryDir(void) { return (klj_val){.l = kl_jni_new_string(g_native_lib_dir)}; }
static klj_val klj_appinfo_dataDir(void)       { return (klj_val){.l = kl_jni_new_string(g_files_dir)}; }
// This APK is not a split install, so there are no additional source dirs. An
// empty array says that unambiguously; Android would say null, and both read as
// "no splits" to a caller that checks length.
static klj_val klj_appinfo_splitSourceDirs(void) {
    static void *empty;
    if (!empty) empty = klj_new_array('L', "java/lang/String", 0);
    return (klj_val){.l = empty};
}

// ClassLoader.findLibrary(name) turns a System.loadLibrary() name into the
// absolute path of the .so inside the APK's native library directory, so a
// caller need not know the path layout. Null is Android's own answer for a
// library that is not there, so a miss needs nothing invented.
static klj_val klj_ClassLoader_findLibrary(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name || !*name) return (klj_val){.l = NULL};
    char   path[1024];
    size_t len = strlen(name);
    if (strncmp(name, "lib", 3) == 0 && len > 3 && strcmp(name + len - 3, ".so") == 0)
        snprintf(path, sizeof path, "%s/%s", g_native_lib_dir, name);
    else
        snprintf(path, sizeof path, "%s/lib%s.so", g_native_lib_dir, name);
    struct stat st;
    int found = stat(path, &st) == 0;
    KLJ_LOG("ClassLoader.findLibrary(\"%s\") -> %s", name, found ? path : "null");
    return (klj_val){.l = found ? kl_jni_new_string(path) : NULL};
}

// No OBB expansion files: this APK carries its assets inline, and there is no
// /Android/obb to point at. An empty array is the real answer for such an app.
static klj_val klj_Context_getObbDirs(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *empty;
    if (!empty) empty = klj_new_array('L', "java/io/File", 0);
    return (klj_val){.l = empty};
}

// Android returns a File here whether or not the directory exists; ours is
// created, which is harmless and keeps any later write working.
static klj_val klj_Context_getObbDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char path[1024];
    snprintf(path, sizeof path, "%s/obb", g_files_dir);
    return (klj_val){.l = klj_new_file(path)};
}

// ---- UI thread queue ----
// runOnUiThread posts to the main looper and returns; it does not run the
// Runnable inline unless already on that thread. Queuing is therefore the
// faithful behaviour, not a shortcut.
//
// kl_jni_drain_ui_tasks() runs them, which is the host->guest direction and the
// only one in this file: every other call here answers something the guest
// started. The delay is recorded but not honoured — a posted task runs at the
// next drain regardless of its delay, which is wrong in the same way a
// zero-latency looper is wrong, and has not mattered yet.
// A queue entry is either a Runnable to run or a Message to deliver to its
// target Handler's callback. One queue, because on Android both land on the same
// main looper and ordering between them is observable.
#define KLJ_MAX_UI_TASKS 64
static struct {
    void   *runnable;      // Runnable, or NULL when this entry is a message
    void   *message;       // android/os/Message, or NULL when it is a runnable
    int64_t delay_ms;
} g_ui_tasks[KLJ_MAX_UI_TASKS];
static unsigned g_ui_task_n;

unsigned kl_jni_pending_ui_tasks(void) { return g_ui_task_n; }

// One queue behind both posting routes — runOnUiThread and Handler.post* target
// the same main-thread looper on Android, so splitting them would only hide half
// the backlog from kl_jni_pending_ui_tasks().
static void klj_ui_enqueue(const char *via, void *runnable, int64_t delay_ms) {
    if (runnable && g_ui_task_n < KLJ_MAX_UI_TASKS) {
        g_ui_tasks[g_ui_task_n].runnable = runnable;
        g_ui_tasks[g_ui_task_n].message  = NULL;
        g_ui_tasks[g_ui_task_n].delay_ms = delay_ms;
        g_ui_task_n++;
    }
    KLJ_LOG("%s: queued (+%lldms, %u pending)", via, (long long)delay_ms, g_ui_task_n);
}

static void klj_msg_enqueue(const char *via, void *message) {
    if (message && g_ui_task_n < KLJ_MAX_UI_TASKS) {
        g_ui_tasks[g_ui_task_n].runnable = NULL;
        g_ui_tasks[g_ui_task_n].message  = message;
        g_ui_tasks[g_ui_task_n].delay_ms = 0;
        g_ui_task_n++;
    }
    KLJ_LOG("%s: queued (%u pending)", via, g_ui_task_n);
}

static klj_val klj_Activity_runOnUiThread(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_ui_enqueue("runOnUiThread", n > 0 ? a[0].l : NULL, 0);
    return (klj_val){0};
}

// The main Looper is an identity, not a mechanism: Unity holds it to build a
// Handler and to ask whether it is already on that thread.
static klj_val klj_Looper_getMainLooper(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *main_looper;
    return klj_singleton("android/os/Looper", &main_looper);
}
// new Handler() binds to the calling thread's Looper; new Handler(looper) to the
// one given. We have a single queue, so the Looper is recorded and not acted on.
// The Callback form keeps the callback: sendToTarget() has to find it again, and
// it is the only thing that gives a Message any meaning.
typedef struct { void *looper, *callback; } klj_handler;

static klj_val klj_Handler_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_handler *h = calloc(1, sizeof *h);
    if (h) {
        h->looper   = n > 0 ? a[0].l : NULL;
        h->callback = n > 1 ? a[1].l : NULL;
    }
    klj_as_object(obj)->data = h;
    if (h && h->callback) {
        klj_object *cb = klj_as_object(h->callback);
        KLJ_LOG("new Handler(looper, callback=%s)", cb ? cb->cls : "(untagged)");
    }
    return (klj_val){.l = obj};
}

static klj_handler *klj_as_handler(void *obj) {
    klj_object *o = klj_as_object(obj);
    return (o && strcmp(o->cls, "android/os/Handler") == 0) ? o->data : NULL;
}
// postDelayed returns whether the message made it into the queue — which it did.
// The delay is recorded rather than honoured; there is no clock driving this
// queue until something drains it.
static klj_val klj_Handler_postDelayed(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_ui_enqueue("Handler.postDelayed", n > 0 ? a[0].l : NULL,
                   n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 1};
}
static klj_val klj_Handler_post(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_ui_enqueue("Handler.post", n > 0 ? a[0].l : NULL, 0);
    return (klj_val){.j = 1};
}

// ---- HandlerThread loopers ----
//
// A HandlerThread is a real thread with a real message loop, and here that turns
// out to be load-bearing rather than a detail worth simplifying away.
//
// The first version of this treated a HandlerThread's Looper as the main one and
// its start() as a no-op, on the reasoning that everything drains through
// kl_jni_drain_ui_tasks() anyway. That is true for Runnables, and wrong for
// Messages: the guest sends a message and then *blocks* waiting for the handler to
// run, which only works if the loop is on another thread. With no such thread the
// main thread sat in __psynch_cvwait until the watchdog fired — a hang whose cause
// was two layers away from where it presented, exactly the failure mode a silent
// no-op produces.
//
// So a started HandlerThread gets a host thread and its own queue. The main looper
// keeps the host-driven drain, because there genuinely is no thread of ours
// running it.
#define KLJ_MAX_LOOPER_MSGS 64
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_t       thread;
    int             running, started;
    void           *q[KLJ_MAX_LOOPER_MSGS];
    unsigned        head, tail, count;
} klj_looper;

static void klj_deliver_message(void *message);

// Guest code runs on this thread, so kl_thread_init() before the first delivery is
// mandatory (S0.1 / trap 1) — without it the stack-protector prologue reads an
// empty TSD slot and the guest dies a long way from here.
static void *klj_looper_thread(void *arg) {
    klj_looper *lp = arg;
    kl_thread_init();
    pthread_mutex_lock(&lp->lock);
    while (lp->running) {
        if (!lp->count) { pthread_cond_wait(&lp->wake, &lp->lock); continue; }
        void *msg = lp->q[lp->head];
        lp->head  = (lp->head + 1) % KLJ_MAX_LOOPER_MSGS;
        lp->count--;
        pthread_mutex_unlock(&lp->lock);
        klj_deliver_message(msg);          // outside the lock: it runs guest code
        pthread_mutex_lock(&lp->lock);
    }
    pthread_mutex_unlock(&lp->lock);
    return NULL;
}

static void klj_looper_post(klj_looper *lp, void *message) {
    pthread_mutex_lock(&lp->lock);
    if (lp->count < KLJ_MAX_LOOPER_MSGS) {
        lp->q[lp->tail] = message;
        lp->tail = (lp->tail + 1) % KLJ_MAX_LOOPER_MSGS;
        lp->count++;
    } else {
        KLJ_LOG("looper queue full — message dropped");
    }
    pthread_cond_signal(&lp->wake);
    pthread_mutex_unlock(&lp->lock);
}

// A Looper object carries the klj_looper it belongs to, or NULL for the main one.
static klj_looper *klj_looper_of(void *looper_obj) {
    klj_object *o = klj_as_object(looper_obj);
    return (o && strcmp(o->cls, "android/os/Looper") == 0) ? o->data : NULL;
}

// ---- android.os.Message ----
//
// A Message is a Runnable's counterpart on the same queue: post() carries code to
// run, sendMessage() carries data for the target Handler's callback to interpret.
// Both end up on the main looper, so they share the queue below rather than
// getting a second one.
//
// What is actually behind this in Beat Saber is Unity's AudioVolumeHandler: it
// registers for volume-change notifications and turns each one into a Message
// whose handler calls the guest native onAudioVolumeChanged(int). Nothing on this
// side changes the volume, so no message is expected to be *sent* — obtainMessage
// is reached during setup regardless. That is why this is deliberately only as
// much machinery as the trace forces: the object and its fields, and no delivery
// path until something is proven to send one.
typedef struct { int32_t what, arg1, arg2; void *obj, *target; } klj_message;

static klj_message *klj_as_message(void *obj) {
    klj_object *o = klj_as_object(obj);
    return (o && strcmp(o->cls, "android/os/Message") == 0) ? o->data : NULL;
}

// obtainMessage(what) — Android recycles these from a pool; we allocate, because
// the pool is an allocation optimisation and nothing observable depends on it.
static klj_val klj_Handler_obtainMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_message *m = calloc(1, sizeof *m);
    if (!m) return (klj_val){.l = NULL};
    m->what   = n > 0 ? (int32_t)a[0].j : 0;
    m->target = self;                       // sendToTarget() needs to find it back
    void *obj = klj_new_object_data("android/os/Message", m);
    klj_as_object(obj)->pinned = 1;   // queued until delivered; outlives its frame

    // Message's members are public *fields*, not getters, so the handler reads
    // msg.what with GetIntField. Publishing them through the same per-object write
    // table the guest's own Set*Field uses means a read finds them by the ordinary
    // path — no instance-field special case, and a guest that writes one back gets
    // the write it expects.
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "what", "I", 'f'),
                    (klj_val){.j = (uint64_t)(int64_t)m->what});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg1", "I", 'f'),
                    (klj_val){.j = 0});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg2", "I", 'f'),
                    (klj_val){.j = 0});
    KLJ_LOG("Handler.obtainMessage(what=%d)", m->what);
    return (klj_val){.l = obj};
}

// sendToTarget() posts the message to the Handler it came from. Never delivered
// inline: Android returns immediately and the looper delivers later, and the
// sender here is frequently about to block waiting for that to happen on another
// thread — delivering inline would run the callback before the sender is ready
// for it, and on the wrong thread.
//
// Which queue depends on the target Handler's Looper. A HandlerThread's looper has
// a thread of its own to deliver on; the main looper does not, so its messages
// wait for the host's drain.
static klj_val klj_Message_sendToTarget(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_message *m = klj_as_message(self);
    if (!m) {
        KLJ_LOG("Message.sendToTarget() on something that is not a Message");
        return (klj_val){.j = 0};
    }
    klj_handler *h  = klj_as_handler(m->target);
    klj_looper  *lp = h ? klj_looper_of(h->looper) : NULL;
    if (lp && lp->started) {
        KLJ_LOG("Message.sendToTarget(what=%d) -> HandlerThread looper", m->what);
        klj_looper_post(lp, self);
    } else {
        klj_msg_enqueue("Message.sendToTarget", self);
    }
    return (klj_val){.j = 0};
}

// ---- android.os.HandlerThread ----
// The thread is not started here — Android requires an explicit start() — so the
// looper exists but nothing delivers on it until then.
static klj_val klj_HandlerThread_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_looper *lp = calloc(1, sizeof *lp);
    if (lp) {
        pthread_mutex_init(&lp->lock, NULL);
        pthread_cond_init(&lp->wake, NULL);
    }
    klj_as_object(obj)->data = lp;
    return (klj_val){.l = obj};
}

// getLooper() blocks on Android until the thread is running; ours is ready as
// soon as start() has spawned it, and callers only use it to build a Handler.
static klj_val klj_HandlerThread_getLooper(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    void *looper = kl_jni_new_object("android/os/Looper");
    klj_as_object(looper)->data = o ? o->data : NULL;    // shares the klj_looper
    return (klj_val){.l = looper};
}

static klj_val klj_HandlerThread_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_looper *lp = o ? o->data : NULL;
    if (!lp) {
        KLJ_LOG("HandlerThread.start() with no looper — nothing to run");
        return (klj_val){.j = 0};
    }
    if (!lp->started) {
        lp->started = lp->running = 1;
        pthread_create(&lp->thread, NULL, klj_looper_thread, lp);
        KLJ_LOG("HandlerThread.start() — looper thread running");
    }
    return (klj_val){.j = 0};
}

// ---- android.view.Choreographer ----
//
// Android's vsync callback, and the engine's frame clock: the guest posts a
// FrameCallback and gets doFrame(frameTimeNanos) once per display refresh. Beat
// Saber reaches it through the Android Game SDK, whose ChoreographerCallback
// registered the native nOnChoreographer(long, long).
//
// getInstance() is per-thread on Android. One instance is enough here because
// nothing distinguishes them: the callback list is what matters, and it is driven
// from the host's frame pump rather than by a real vsync source.
static klj_val klj_Choreographer_getInstance(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *instance;
    return klj_singleton("android/view/Choreographer", &instance);
}

// A posted frame callback is *one-shot* on Android: it fires at the next frame and
// is then forgotten, and a caller that wants every frame re-posts from inside
// doFrame. Keeping that exactly right matters — treating it as persistent would
// call it twice per frame once the guest re-posts, and the engine derives its
// delta time from the gap between calls.
static void *g_frame_callback;

static klj_val klj_Choreographer_postFrameCallback(void *env, void *self,
                                                   const klj_val *a, int n) {
    (void)env; (void)self;
    g_frame_callback = n > 0 ? a[0].l : NULL;
    if (g_frame_callback) {
        static int announced;
        if (!announced) {
            announced = 1;
            klj_object *o = klj_as_object(g_frame_callback);
            KLJ_LOG("Choreographer.postFrameCallback(%s) — the frame clock is now "
                    "driven from the host's pump", o ? o->cls : "(untagged)");
        }
    }
    return (klj_val){.j = 0};
}

// kl_jni_tick_choreographer() lives further down, next to the message delivery —
// both call into guest proxies, and that machinery is defined there.

// ---- android.os.Process ----
// setThreadPriority(tid, priority) is a no-op we can only record. Android's
// priority is a Linux nice value applied to another thread by tid; Darwin has no
// equivalent — scheduling is set through pthread QoS classes on the thread
// itself, so honouring this would mean intercepting it at thread creation. It is
// logged rather than silently dropped because it names which of the engine's
// threads expect to run below normal.
static klj_val klj_Process_setThreadPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n > 1) KLJ_LOG("Process.setThreadPriority(tid=%d, %d) — not applied on Darwin",
                       (int)a[0].j, (int)a[1].j);
    else if (n > 0) KLJ_LOG("Process.setThreadPriority(%d) — not applied on Darwin",
                            (int)a[0].j);
    return (klj_val){0};
}
static klj_val klj_Process_myTid(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (klj_val){.j = (uint32_t)tid};
}

// ---- the presented display ----
// The screen itself is described up at KLJ_DISPLAY_*, next to g_fields, since
// most of it is read as DisplayMetrics fields rather than through a method.

// Display.DEFAULT_DISPLAY is 0. There is exactly one display here, so any other
// id has no Display — which is Android's own answer, not a shortcut.
static klj_val klj_DisplayManager_getDisplay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int32_t id = n > 0 ? (int32_t)a[0].j : 0;
    if (id != 0) {
        KLJ_LOG("DisplayManager.getDisplay(%d) -> null (only display 0 exists)", id);
        return (klj_val){.l = NULL};
    }
    static void *display;
    return klj_singleton("android/view/Display", &display);
}

// registerDisplayListener is a host->guest callback, and it lands in the same
// gap runOnUiThread does: we can record the listener but nothing will ever fire
// it, because calling into the proxy needs JNIBridge.invoke. Our display never
// changes, so no callback is *owed* — but that is a property of the display we
// chose, not of the mechanism, so it is logged rather than left silent.
static klj_val klj_DisplayManager_registerDisplayListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("DisplayManager.registerDisplayListener(%p) — recorded; the presented "
            "display never changes, so nothing is owed a callback",
            n > 0 ? a[0].l : NULL);
    return (klj_val){0};
}
static klj_val klj_DisplayManager_unregisterDisplayListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};
}

// Everything below reads straight off KLJ_DISPLAY_*. getWidth/getHeight are the
// deprecated pair and getRealMetrics the modern one; they agree here because our
// display has no system decor to subtract, which is the whole reason the two
// exist separately on Android.
static klj_val klj_Display_getDisplayId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_Display_getWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_W};
}
static klj_val klj_Display_getHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_H};
}
static klj_val klj_Display_getRotation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_ROTATION};
}
static klj_val klj_Display_getRefreshRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.d = KLJ_DISPLAY_REFRESH};
}
// The DisplayMetrics the guest passes is already answered from the same
// constants by the field dispatch, so there is nothing to write into it — one
// display means every DisplayMetrics describes it. Logged once so the no-op is
// visible if the metrics ever stop matching what Unity then computes.
static klj_val klj_Display_getMetrics(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++) KLJ_LOG("Display.get*Metrics: %dx%d @ %ddpi, %.0f Hz (fields are constants)",
                         KLJ_DISPLAY_W, KLJ_DISPLAY_H, KLJ_DISPLAY_DPI, (double)KLJ_DISPLAY_REFRESH);
    return (klj_val){0};
}
// Frame-pacing numbers. Unity's FrameTiming asks for these to decide when to
// submit; they have to be consistent with the refresh rate we claim or it will
// pace against a period that does not exist. The vsync offset is 0 on nearly
// every real device, and the presentation deadline is surfaceflinger's default
// of one refresh period plus a millisecond.
static klj_val klj_Display_getAppVsyncOffsetNanos(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_Display_getPresentationDeadlineNanos(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)(1e9 / KLJ_DISPLAY_REFRESH) + 1000000};
}
// sRGB, not wide gamut — which is what the panel description above says, and
// claiming otherwise would have Unity pick a colour space it then renders into.
static klj_val klj_Display_isWideColorGamut(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Resources.getIdentifier looks a name up in the APK's resource table. We have
// no resource table, and 0 is Android's own "no such resource" — the caller must
// already handle it, since a name that is not in the APK returns the same thing.
// Logged, because it names what the guest expected to find.
static klj_val klj_Resources_getIdentifier(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Resources.getIdentifier(\"%s\", \"%s\", \"%s\") -> 0 (no resource table)",
            n > 0 ? klj_str(a[0].l) : "", n > 1 ? klj_str(a[1].l) : "",
            n > 2 ? klj_str(a[2].l) : "");
    return (klj_val){.j = 0};
}

static klj_val klj_Integer_parseInt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *t = n > 0 ? klj_str(a[0].l) : NULL;
    // Java throws NumberFormatException on garbage; nothing here can throw, and
    // the only caller is FMOD reading back the audio properties we just handed
    // it, so the input is our own string. strtol's 0 is the right answer for
    // anything else and the log says when that happened.
    long v = t ? strtol(t, NULL, 10) : 0;
    if (!t) KLJ_LOG("Integer.parseInt(null) -> 0");
    return (klj_val){.j = (uint64_t)(int64_t)(int32_t)v};
}


// ---------------------------------------------------------------- JNIBridge
//
// This is the only direction that runs host -> guest. Everything else in this
// file answers a call the guest made; here we originate one.
//
// Unity's bitter/jnibridge represents a Java object implementing some interface
// as a *proxy*: the guest calls JNIBridge.newInterfaceProxy(nativePtr,
// Class[]) — which is ours to implement — and gets back an object it can hand
// to anything expecting, say, a Runnable. Invoking a method on that proxy means
// calling the guest's own registered native
//
//   JNIBridge.invoke(long ptr, Class declaringClass, Method method, Object[] args)
//
// so the proxy has to remember the pointer, and we have to be able to
// manufacture a java.lang.reflect.Method describing what is being called.
// Neither object exists anywhere until we make it.
// A java.lang.reflect.Method the guest never created. What its native invoke
// actually reads off this is not knowable up front, so the accessors below are
// added as the trace demands them — same rule as every other Java class here.
static void *klj_new_method(const char *cls, const char *name, const char *sig) {
    klj_method_obj *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cls = cls; m->name = name; m->sig = sig;
    return klj_new_object_data(KLJ_CLASS_METHOD, m);
}

static klj_val klj_Method_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_method_obj *m = o ? o->data : NULL;
    return (klj_val){.l = kl_jni_new_string(m ? m->name : "")};
}

// ------------------------------------------------- Unity's ReflectionHelper
//
// com/unity3d/player/ReflectionHelper is Unity's own Java helper, and its field
// half is a three-step round trip: getFieldID(class, name, sig, isStatic) hands
// back a java.lang.reflect.Field, getFieldSignature reads the signature back off
// it, and FromReflectedField turns it into the jfieldID the guest actually uses.
//
// All three are implemented together because any one alone is inert — a Field you
// cannot query or convert is not a partial answer, it is a dead end. That is the
// same reasoning as the other group answers, applied to a round trip rather than
// to a set of properties.
//
// The strings are copied, not aliased: they arrive as guest jstrings and trap 6
// is exactly this mistake made once already with RegisterNatives.
static void *klj_new_field(const char *cls, const char *name, const char *sig, int is_static) {
    klj_field_obj *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->cls = cls ? strdup(cls) : NULL;
    f->name = name ? strdup(name) : NULL;
    f->sig = sig ? strdup(sig) : NULL;
    f->is_static = is_static;
    return klj_new_object_data(KLJ_CLASS_FIELD, f);
}

static klj_field_obj *klj_as_field(void *obj) {
    klj_object *o = klj_as_object(obj);
    return (o && strcmp(o->cls, KLJ_CLASS_FIELD) == 0) ? o->data : NULL;
}

static klj_val klj_ReflectionHelper_getFieldID(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *cls  = n > 0 ? klj_class_name(a[0].l) : NULL;
    const char *name = n > 1 ? klj_str(a[1].l) : NULL;
    const char *sig  = n > 2 ? klj_str(a[2].l) : NULL;
    int   is_static  = n > 3 ? (int)a[3].j : 0;
    if (!cls || !name) {
        KLJ_LOG("ReflectionHelper.getFieldID with no class or name -> null");
        return (klj_val){.l = NULL};
    }
    KLJ_LOG("ReflectionHelper.getFieldID %s.%s%s%s", cls, name, sig ? " " : "",
            sig ? sig : "");
    return (klj_val){.l = klj_new_field(cls, name, sig, is_static)};
}

// The method half of the same round trip. Unity looks a method up by name and
// signature and gets back a java.lang.reflect.Method it then converts with
// FromReflectedMethod — which is why is_static is carried on the object rather
// than assumed: a static method has to come back as a static id.
static klj_val klj_ReflectionHelper_getMethodID(void *env, void *self,
                                                const klj_val *a, int n) {
    (void)env; (void)self;
    const char *cls  = n > 0 ? klj_class_name(a[0].l) : NULL;
    const char *name = n > 1 ? klj_str(a[1].l) : NULL;
    const char *sig  = n > 2 ? klj_str(a[2].l) : NULL;
    int   is_static  = n > 3 ? (int)a[3].j : 0;
    if (!cls || !name) {
        KLJ_LOG("ReflectionHelper.getMethodID with no class or name -> null");
        return (klj_val){.l = NULL};
    }
    KLJ_LOG("ReflectionHelper.getMethodID %s.%s%s", cls, name, sig ? sig : "");
    void *m = klj_new_method(strdup(cls), name ? strdup(name) : NULL,
                             sig ? strdup(sig) : NULL);
    klj_object *o = klj_as_object(m);
    if (o && o->data) ((klj_method_obj *)o->data)->is_static = is_static;
    return (klj_val){.l = m};
}

static klj_val klj_ReflectionHelper_getFieldSignature(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self;
    klj_field_obj *f = n > 0 ? klj_as_field(a[0].l) : NULL;
    if (!f) {
        KLJ_LOG("ReflectionHelper.getFieldSignature on something that is not a Field");
        return (klj_val){.l = NULL};
    }
    return (klj_val){.l = kl_jni_new_string(f->sig ? f->sig : "")};
}

// The mirror of FromReflectedMethod: recover the id from the description the
// Field was built out of, so the round trip lands on the same klj_wanted entry a
// direct GetFieldID would have produced.
static void *klj_FromReflectedField(void *env, void *field) {
    (void)env;
    klj_field_obj *f = klj_as_field(field);
    if (!f) {
        KLJ_LOG("FromReflectedField: not a Field");
        kl_jni_report(stderr);
        kl_fatal_prepare(); abort();
    }
    return klj_want(klj_class_object(f->cls), f->name, f->sig ? f->sig : "",
                    f->is_static ? 'F' : 'f');
}

static klj_val klj_Field_getDeclaringClass(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_field_obj *f = klj_as_field(self);
    return (klj_val){.l = f ? klj_class_object(f->cls) : NULL};
}

static klj_val klj_Method_getDeclaringClass(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_method_obj *m = o ? o->data : NULL;
    return (klj_val){.l = m ? klj_class_object(m->cls) : NULL};
}

// Call one method on a JNIBridge proxy. Returns the guest's return value, or
// NULL if there is nothing to call — which is not an error here: a queue can
// legitimately hold something that is not a proxy, and refusing loudly at drain
// time would turn a diagnostic into a crash.
static void *klj_proxy_invoke(void *proxy, const char *iface,
                              const char *name, const char *sig, void *args) {
    klj_object *o = klj_as_object(proxy);
    if (!o || strcmp(o->cls, KLJ_CLASS_PROXY) != 0) {
        KLJ_LOG("drain: %s is not a JNIBridge proxy — skipped",
                o ? o->cls : "(not an object)");
        return NULL;
    }
    void *fn = kl_jni_native("bitter/jnibridge/JNIBridge", "invoke", NULL);
    if (!fn) {
        KLJ_LOG("drain: the guest has not registered JNIBridge.invoke — skipped");
        return NULL;
    }
    klj_proxy *p = o->data;
    // A static native is (JNIEnv*, jclass, args...). The frame pair is the
    // JVM's pop on native return — the method object, the args array, and
    // whatever the guest allocates inside the callback all die at the pop.
    void *(*invoke)(void *, void *, int64_t, void *, void *, void *) = fn;
    klj_PushLocalFrame(NULL, 0);
    void *r = invoke(kl_jni_env(), klj_class_object("bitter/jnibridge/JNIBridge"),
                     p->native_ptr, klj_class_object(iface),
                     klj_new_method(iface, name, sig), args);
    klj_PopLocalFrame(NULL, NULL);
    return r;
}

// Run everything the guest posted to the main looper. On Android this is what
// Looper.loop() does on the UI thread; here the host has to call it, because
// there is no looper thread and the queue would otherwise only ever grow.
//
// The queue is snapshotted and cleared before anything runs: a Runnable is guest
// code and is free to post more work, and draining in place would either miss
// those or spin on them forever.
// Deliver one Message to its target Handler's callback.
//
// Two shapes of callback are possible and they are dispatched differently. A
// JNIBridge proxy is guest code implementing Handler.Callback, and goes through
// the usual proxy path. But the callback Unity actually installs here is one of
// its *own Java* classes — AudioVolumeHandler — which we do not have, so standing
// in for it means doing what its handleMessage does: forward the message to the
// native it registered. That native's pointer is already in hand from
// RegisterNatives, so this is a lookup rather than an invention.
//
// Anything else is named and dropped rather than silently discarded: a message
// that goes nowhere is a notification the guest is still waiting for.
static void klj_deliver_message(void *message) {
    klj_message *m = klj_as_message(message);
    if (!m) return;
    klj_handler *h  = klj_as_handler(m->target);
    void       *cb  = h ? h->callback : NULL;
    klj_object *cbo = cb ? klj_as_object(cb) : NULL;

    if (cbo && strcmp(cbo->cls, KLJ_CLASS_PROXY) == 0) {
        void *args = klj_new_array('L', "java/lang/Object", 1);
        ((void **)klj_arr(args)->data)[0] = message;
        klj_proxy_invoke(cb, "android/os/Handler$Callback", "handleMessage",
                         "(Landroid/os/Message;)Z", args);
        return;
    }

    if (cbo && strcmp(cbo->cls, "com/unity3d/player/AudioVolumeHandler") == 0) {
        void *fn = kl_jni_native("com/unity3d/player/AudioVolumeHandler",
                                 "onAudioVolumeChanged", NULL);
        if (!fn) {
            KLJ_LOG("message for AudioVolumeHandler, but its native is not registered");
            return;
        }
        KLJ_LOG("delivering message what=%d -> AudioVolumeHandler.onAudioVolumeChanged",
                m->what);
        ((void (*)(void *, void *, kl_jint))fn)(kl_jni_env(), cb, (kl_jint)m->what);
        return;
    }

    KLJ_LOG("message what=%d has no deliverable callback (target callback is %s) — "
            "DROPPED. If the guest is waiting on this notification, implement it here.",
            m->what, cbo ? cbo->cls : "(none)");
}

// Box a long for JNIBridge.invoke, which takes its arguments as an Object[].
static void *klj_box_long(int64_t v) {
    klj_pref *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->kind = 'J';
    e->ival = v;
    void *obj = kl_jni_new_object("java/lang/Long");
    klj_as_object(obj)->data = e;
    return obj;
}

// Fire one frame's callback, if one is pending. Called from the host's frame pump
// — there is no vsync source here, so "a frame" is whatever the pump says it is.
void kl_jni_tick_choreographer(void) {
    void *cb = g_frame_callback;
    if (!cb) return;
    g_frame_callback = NULL;               // one-shot; doFrame may re-post

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t nanos = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;

    klj_object *o = klj_as_object(cb);
    if (o && strcmp(o->cls, KLJ_CLASS_PROXY) == 0) {
        // The frame covers the args array and the boxed nanos too — they are
        // per-call locals, and 20000 frames of leaking them is what the pool
        // exhaustion looked like.
        klj_PushLocalFrame(NULL, 0);
        void *args = klj_new_array('L', "java/lang/Object", 1);
        ((void **)klj_arr(args)->data)[0] = klj_box_long(nanos);
        klj_proxy_invoke(cb, "android/view/Choreographer$FrameCallback", "doFrame",
                         "(J)V", args);
        klj_PopLocalFrame(NULL, NULL);
        return;
    }
    KLJ_LOG("frame callback is %s, not a proxy — doFrame NOT delivered. The engine "
            "will see no frame clock; implement this dispatch.",
            o ? o->cls : "(untagged)");
}

unsigned kl_jni_drain_ui_tasks(void) {
    struct { void *runnable, *message; } batch[KLJ_MAX_UI_TASKS];
    unsigned n;
    pthread_mutex_lock(&g_lock);
    n = g_ui_task_n;
    for (unsigned i = 0; i < n; i++) {
        batch[i].runnable = g_ui_tasks[i].runnable;
        batch[i].message  = g_ui_tasks[i].message;
    }
    g_ui_task_n = 0;
    pthread_mutex_unlock(&g_lock);

    if (n) KLJ_LOG("draining %u posted task%s", n, n == 1 ? "" : "s");
    for (unsigned i = 0; i < n; i++) {
        if (batch[i].message) klj_deliver_message(batch[i].message);
        else klj_proxy_invoke(batch[i].runnable, "java/lang/Runnable", "run", "()V", NULL);
    }
    return n;
}



// The system-UI visibility group. Answered as a pair rather than call by call:
// Unity sets a flag word and reads it straight back to confirm, so a setter that
// discarded the value would make it retry forever.
static int32_t g_system_ui_visibility;

static klj_val klj_View_setSystemUiVisibility(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    g_system_ui_visibility = n > 0 ? (int32_t)a[0].j : 0;
    KLJ_LOG("View.setSystemUiVisibility(0x%x) — recorded; there is no navigation "
            "bar here to hide", g_system_ui_visibility);
    return (klj_val){.j = 0};
}

static klj_val klj_View_getSystemUiVisibility(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)(int64_t)g_system_ui_visibility};
}

static klj_val klj_View_setSysUiListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Recorded, and nothing is owed a callback: our visibility only ever changes
    // because the guest changed it, and Android does not report those back.
    KLJ_LOG("View.setOnSystemUiVisibilityChangeListener — recorded, never fires");
    return (klj_val){.j = 0};
}

// ------------------------------------------------------ window flags / input
//
// The batch nativeRender reaches after audio. Mostly constants, and the methods
// that consume them are recorded rather than applied: there is no window manager
// here to keep a screen on or hide a navigation bar, and pretending otherwise
// would be inventing behaviour rather than reporting it.
static klj_val klj_Window_getDecorView(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *view;
    return klj_singleton("android/view/View", &view);
}

static klj_val klj_Window_setFlags(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Window.setFlags(0x%llx, mask 0x%llx) — recorded, not applied",
            n > 0 ? (unsigned long long)a[0].j : 0ull,
            n > 1 ? (unsigned long long)a[1].j : 0ull);
    return (klj_val){.j = 0};
}

static klj_val klj_PowerManager_sustainedPerf(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Answered false because it is true: nothing here honours a sustained
    // performance request, and claiming it would have Unity size its frame
    // budget against a guarantee we do not make.
    return (klj_val){.j = 0};
}

static klj_val klj_InputDevice_getDeviceIds(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Empty: no controllers are attached, which is the truth until M7 binds
    // GameController. An invented id would have Unity poll a device that cannot
    // answer.
    static void *empty;
    if (!empty) empty = klj_new_array('I', NULL, 0);
    KLJ_LOG("InputDevice.getDeviceIds() -> [] (no input devices until M7)");
    return (klj_val){.l = empty};
}

static klj_val klj_InputManager_registerListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Same shape as registerDisplayListener: recorded, and nothing is owed a
    // callback because our device set never changes.
    KLJ_LOG("InputManager.registerInputDeviceListener — recorded; the device set never changes");
    return (klj_val){.j = 0};
}

static klj_val klj_AudioManager_getStreamVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    // Android's music stream runs 0..15 and Unity scales its mixer against it.
    // Full volume is the honest answer for a host that has no volume control of
    // its own to report.
    KLJ_LOG("AudioManager.getStreamVolume(%lld) -> 15 (of 15)",
            n > 0 ? (long long)(int32_t)a[0].j : -1);
    return (klj_val){.j = 15};
}

static klj_val klj_UnityPlayer_getLaunchURL(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // null, and that is the answer rather than a placeholder: the app was not
    // launched from a deep link, and Unity checks for null explicitly.
    return (klj_val){.l = NULL};
}

// ---------------------------------------------------------------- audio
//
// Answered as one description of an audio device rather than call by call, for
// the same reason as the display group: Unity's FMOD asks for the sample rate
// and the buffer size separately and then sizes its mixer from both, so two
// answers that disagree are worse than either alone. 48 kHz / 256 frames is the
// low-latency configuration a Quest 2 reports, and it is what the engine had
// already inferred from the driver before it got this far.
#define KLJ_AUDIO_RATE   48000
#define KLJ_AUDIO_FRAMES 256

static klj_val klj_AudioManager_getProperty(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    // The keys are the values of AudioManager.PROPERTY_* below; match on the
    // documented strings so the two cannot drift apart.
    if (strstr(k, "OUTPUT_SAMPLE_RATE")) {
        KLJ_LOG("AudioManager.getProperty(OUTPUT_SAMPLE_RATE) -> %d", KLJ_AUDIO_RATE);
        return (klj_val){.l = kl_jni_new_string("48000")};
    }
    if (strstr(k, "OUTPUT_FRAMES_PER_BUFFER")) {
        KLJ_LOG("AudioManager.getProperty(OUTPUT_FRAMES_PER_BUFFER) -> %d", KLJ_AUDIO_FRAMES);
        return (klj_val){.l = kl_jni_new_string("256")};
    }
    // Android returns null for an unknown property, and FMOD handles that.
    KLJ_LOG("AudioManager.getProperty(\"%s\") -> null (unknown property)", k);
    return (klj_val){.l = NULL};
}

static klj_val klj_PackageManager_hasSystemFeature(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *f = n > 0 ? klj_str(a[0].l) : "";
    // Truthfully, for the device we present: a Quest 2 reports low-latency audio
    // and it is the branch FMOD wants. Anything else is answered no rather than
    // guessed yes — claiming a feature is a promise, same rule as the EGL and GL
    // extension strings.
    int has = strstr(f, "audio.low_latency") != NULL;
    KLJ_LOG("PackageManager.hasSystemFeature(\"%s\") -> %s", f, has ? "true" : "false");
    return (klj_val){.j = (uint64_t)has};
}

static klj_val klj_Context_checkPermission(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Context.checkCallingOrSelfPermission(\"%s\") -> GRANTED",
            n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};      // PackageManager.PERMISSION_GRANTED
}

// ---- javax.net.ssl ----
// The game's HTTPS stack (unitytls) resolves TrustManagerFactory during boot.
// On Android these calls hand back the *system* trust store, which does not
// exist here — and nothing in this runtime can throw, so a failed validation
// could not be reported the Java way anyway. The honest surface is: the
// documented algorithm name, a real factory object, and zero trust managers.
// unitytls reads "no trust managers" as UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED
// and the game takes its own Curl-error path (observed under KL_PERMISSIVE:
// "Curl error 51" and boot continues). Returning a trust-all manager instead
// would silently weaken the guest's TLS validation — refused by the same rule
// as the DRM guard: don't invent answers the guest trusts.
static klj_val klj_TMF_getDefaultAlgorithm(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Android's documented default since API 24 (we present SDK 29).
    return (klj_val){.l = kl_jni_new_string("PKIX")};
}
static klj_val klj_TMF_getInstance(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("TrustManagerFactory.getInstance(\"%s\")",
            n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.l = klj_new_object_data("javax/net/ssl/TrustManagerFactory", NULL)};
}
static klj_val klj_TMF_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};             // init(NULL): "system default store" — absent here
}
static klj_val klj_TMF_getTrustManagers(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L', "javax/net/ssl/TrustManager", 0)};
}


// Window.getAttributes returns the live LayoutParams — the same object every
// time, because on Android the caller mutates it and hands it back through
// setAttributes. One instance is what makes that round trip mean anything.
static klj_val klj_Window_getAttributes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *lp;
    return klj_singleton("android/view/WindowManager$LayoutParams", &lp);
}

// Resources and Window are identities: Unity holds them to reach the display
// metrics and the window flags, both of which we answer from KLJ_DISPLAY_*.
static klj_val klj_Context_getResources(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *resources;
    return klj_singleton("android/content/res/Resources", &resources);
}
static klj_val klj_Activity_getWindow(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *window;
    return klj_singleton("android/view/Window", &window);
}

// setRequestedOrientation is recorded, not applied — there is no window manager
// to rotate anything. It is worth naming in the log because it says which way
// round the engine believes the screen is, which is the first thing to check if
// the render target ever comes out transposed.
static klj_val klj_Activity_setRequestedOrientation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static const char *names[] = {
        "LANDSCAPE", "PORTRAIT", "USER", "BEHIND", "SENSOR", "NOSENSOR",
        "SENSOR_LANDSCAPE", "SENSOR_PORTRAIT", "REVERSE_LANDSCAPE",
        "REVERSE_PORTRAIT", "FULL_SENSOR", "USER_LANDSCAPE", "USER_PORTRAIT",
        "FULL_USER", "LOCKED",
    };
    int32_t o = n > 0 ? (int32_t)a[0].j : -1;
    const char *name = (o >= 0 && o < (int32_t)(sizeof names / sizeof names[0]))
                       ? names[o] : (o == -1 ? "UNSPECIFIED" : "?");
    KLJ_LOG("Activity.setRequestedOrientation(%d /* %s */) — recorded, not applied", o, name);
    return (klj_val){0};
}

// ---- odds and ends the same batch reached for ----
void *klb_dlopen(const char *path, int flags);   // kl_dl.c — the guest's own dlopen

// UnityPlayer.loadLibrary(name) is Unity's System.loadLibrary wrapper. Routing it
// through the guest dlopen rather than kl_load keeps one image registry: the
// library may already be open (libil2cpp arrives this way *and* through
// ClassLoader.findLibrary), and klb_dlopen returns the existing image for that.
static klj_val klj_UnityPlayer_loadLibrary(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name || !*name) return (klj_val){.j = 0};
    char   path[1024];
    size_t len = strlen(name);
    if (strncmp(name, "lib", 3) == 0 && len > 3 && strcmp(name + len - 3, ".so") == 0)
        snprintf(path, sizeof path, "%s/%s", g_native_lib_dir, name);
    else
        snprintf(path, sizeof path, "%s/lib%s.so", g_native_lib_dir, name);
    void *h = klb_dlopen(path, 0x00002 /* RTLD_NOW */);
    KLJ_LOG("UnityPlayer.loadLibrary(\"%s\") -> %s", name, h ? "loaded" : "failed");
    return (klj_val){.j = h != NULL};
}

// System.load(path) is the absolute-path form of loadLibrary — OVRPlugin's C# side
// calls it with whatever ClassLoader.findLibrary returned. Routed through the same
// guest dlopen as UnityPlayer.loadLibrary so there is still one image registry;
// the library is usually already open by the time this runs, and klb_dlopen
// returns the existing image rather than loading it twice.
//
// Returns void, so a failure here cannot be reported to the guest as a value. Real
// Android would throw UnsatisfiedLinkError; we log instead, because the guest goes
// on to dlsym the library and that path already names what it could not resolve.
static klj_val klj_System_load(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *path = n > 0 ? klj_str(a[0].l) : NULL;
    if (!path || !*path) return (klj_val){.j = 0};
    void *h = klb_dlopen(path, 0x00002 /* RTLD_NOW */);
    KLJ_LOG("System.load(\"%s\") -> %s", path, h ? "loaded" : "failed");
    return (klj_val){.j = 0};
}

// The output devices AudioManager knows about. Answered as an empty array, which
// is the one honest answer available: we do not enumerate host audio hardware, and
// every field of an AudioDeviceInfo — id, type, sample rates, channel masks —
// would have to be invented to fill even one entry.
//
// Empty is not a silent zero of the trap-6d kind. Unity uses this list to notice
// device *changes* (headphones, Bluetooth) and to pick a non-default output; with
// nothing to choose from it stays on the default path, which is the OpenSL device
// that already works. If something later needs a populated list, the trace will
// name the AudioDeviceInfo getter it calls rather than leaving us to guess.
static klj_val klj_AudioManager_getDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AudioManager.getDevices() -> empty (host devices are not enumerated)");
    return (klj_val){.l = klj_new_array('L', "android/media/AudioDeviceInfo", 0)};
}

// MediaRouter.getSelectedRoute(type). Android always returns a route here — the
// default one if nothing else is selected — so null would be the invented answer,
// not the conservative one. The RouteInfo we hand back has no presentation display
// attached, which is exactly the ordinary case for a device with nothing plugged
// into it, and is what sends Unity down its normal single-display path.
static klj_val klj_MediaRouter_getSelectedRoute(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *route;
    if (!route) route = kl_jni_new_object("android/media/MediaRouter$RouteInfo");
    return (klj_val){.l = route};
}

// Settings.Secure.getString(resolver, "android_id") is what Unity turns into
// SystemInfo.deviceUniqueIdentifier. It has to be a real string: a null here is
// not a harmless blank, it is the guest aborting inside its own string handling —
// which is exactly how this presented under KL_PERMISSIVE, as a SIGABRT with no
// JNI call named because the permissive zero *was* the answer.
//
// The value is synthetic and constant: Android's own android_id is a per-device,
// per-app-signing-key random 64-bit value, so any 16-hex-digit string is a
// well-formed one. Stable across runs because callers cache it and compare.
#define KLJ_ANDROID_ID "4b6c6570746f6e01"      /* "Klepton\1" as hex */

static klj_val klj_Context_getContentResolver(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *resolver;
    if (!resolver) resolver = kl_jni_new_object("android/content/ContentResolver");
    return (klj_val){.l = resolver};
}

static klj_val klj_Settings_Secure_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *key = n > 1 ? klj_str(a[1].l) : NULL;
    if (key && strcmp(key, "android_id") == 0)
        return (klj_val){.l = kl_jni_new_string(KLJ_ANDROID_ID)};
    // Android returns null for a setting that is not present, and callers are
    // written for it. Naming it keeps an unexpected key from looking like a value.
    KLJ_LOG("Settings.Secure.getString(\"%s\") -> null (not a setting we serve)",
            key ? key : "(null)");
    return (klj_val){.l = NULL};
}

// Thread.start(). This one is deliberately not a plain no-op, because a no-op
// here is the one shape that lies: if the thread carried a guest Runnable, doing
// nothing means its run() never happens and the guest waits forever for work that
// was never started — a hang with no cause anywhere near it.
//
// What we can serve is the case where the "thread" is one of ours: a HandlerThread
// whose work drains through kl_jni_drain_ui_tasks anyway, so there is genuinely
// nothing to start. So the receiver's class is logged every time, and anything
// that is not a HandlerThread is called out as unhandled rather than swallowed.
static klj_val klj_Thread_start(void *env, void *self, const klj_val *a, int n) {
    klj_object *o = klj_as_object(self);
    // Reached through the superclass walk when the guest looks start() up on
    // java/lang/Thread but the receiver is really a HandlerThread — which does
    // have something to start, and the first version of this wrongly said it did
    // not. That produced a guest blocked forever on a message no thread would
    // ever deliver.
    if (o && strcmp(o->cls, "android/os/HandlerThread") == 0)
        return klj_HandlerThread_start(env, self, a, n);

    (void)a; (void)n;
    KLJ_LOG("Thread.start() on %s — NOT started. If this carried a Runnable, its "
            "run() will never execute; that would show up as a wait with no cause. "
            "Implement it here if so.", o ? o->cls : "(not one of our objects)");
    return (klj_val){.j = 0};
}

// A void method whose effect is on state we do not model. Shared, but only ever
// bound to methods that genuinely return void — the arguments are ignored, so
// binding it to something with a return value would hand the guest a zero it
// would read as an answer (trap 6b, which was exactly a handler reused for the
// wrong shape).
static klj_val klj_void_noop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Uri.decode — percent-decoding, implemented rather than stubbed because it is a
// pure function with one right answer.
//
// Note it is *not* URLDecoder.decode: Android's Uri.decode leaves '+' alone rather
// than turning it into a space. Getting that backwards would silently corrupt any
// path containing a plus, which is the kind of bug that surfaces as a missing file
// a long way from here.
static int klj_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static klj_val klj_Uri_decode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    if (!s) return (klj_val){.l = NULL};
    size_t len = strlen(s);
    char  *out = malloc(len + 1);
    if (!out) return (klj_val){.l = NULL};
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < len &&
            (hi = klj_hexval(s[i + 1])) >= 0 && (lo = klj_hexval(s[i + 2])) >= 0) {
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    klj_val r = {.l = kl_jni_new_string(out)};
    free(out);
    return r;
}

// The single UnityPlayerActivity. t_boot hands this same object to initJni as the
// Context, and Unity reads it back through the static UnityPlayer.currentActivity
// — so it has to be one instance, not two of the same class. Created lazily
// because whichever of the two paths runs first should win, and they are the same
// object either way.
void *kl_jni_activity(void) {
    static void *activity;
    if (!activity) {
        activity = kl_jni_new_object("com/unity3d/player/UnityPlayerActivity");
        ((klj_object *)activity)->pinned = 1;   // singleton; survives any frame pop
    }
    return activity;
}

static klj_val klj_currentActivity_field(void) {
    return (klj_val){.l = kl_jni_activity()};
}

// Oculus's device-config client. The native half of this
// (libdeviceconfigclient-full-aar.so) is not in the APK — the dlopen for it fails
// early in every run — so there is no configuration service behind this on a real
// Quest either unless the system provides one. Doing nothing is what an absent
// service does; it is a void method, so there is no answer to invent.
static klj_val klj_OculusDeviceConfig_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("OculusDeviceConfig.init() — no device-config service here");
    return (klj_val){.j = 0};
}

// getCurrentState returns a state enum whose numbering is not recoverable from
// the APK (the class lives in an absent AAR) — but the guest's
// OculusDeviceConfigExperimentModel::<Initialize>d__6::MoveNext disassembly
// (2026-08-07) pins the contract: 2 = success (Initialize completes), 3 =
// failure (GetError() -> System.Exception), anything else = poll
// Task.Delay(11) until a 5 s Stopwatch deadline throws TimeoutException and
// il2cpp aborts. In practice BOTH non-success outcomes abort the boot: 0 polls
// to the watchdog, and the state-3 exception propagates out of
// OculusDeviceConfigExperimentModel::.ctor uncaught (constructed on a
// class-init path — il2cpp aborts on a throwing cctor). That leaves 2: the
// service "succeeded" and the experiment flags it gates read as defaults
// (absent param -> false/0), which is the baseline the game ships for a
// device with no config.
static klj_val klj_OculusDeviceConfig_getCurrentState(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int warned;
    if (!warned) {
        warned = 1;
        KLJ_LOG("OculusDeviceConfig.getCurrentState() -> 2 (success; 0 polls to "
                "the 5 s watchdog and 3's exception is uncatchable where .ctor "
                "runs — enum semantics from the MoveNext disasm)");
    }
    return (klj_val){.j = 2};
}

// The state==2 path queries experiment params one by one. The managed flow
// (GetBooleanAsync disasm, 2026-08-07) is: DidPrefetchParamName(name) -> if
// FALSE the game throws KeyNotFoundException itself and the boot aborts; if
// true it calls GetBoolean(name) and takes our word for the value. There is
// no config backend, so every param is "present" with the value false —
// experiment off, the shipping baseline for a device with no config.
static klj_val klj_OculusDeviceConfig_didPrefetchParamName(void *env, void *self,
                                                           const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("OculusDeviceConfig.didPrefetchParamName(\"%s\") -> true (false is "
            "fatal: the game KeyNotFound-aborts on unknown params)",
            n > 0 ? klj_str(a[0].l) : "?");
    return (klj_val){.j = 1};
}
static klj_val klj_OculusDeviceConfig_getBoolean(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env; (void)self;
    // (Activity, String) — the activity is unused; the name is a[1].
    KLJ_LOG("OculusDeviceConfig.getBoolean(\"%s\") -> false (no config backend)",
            n > 1 ? klj_str(a[1].l) : "?");
    return (klj_val){.j = 0};
}
// The state==3 path reads one error string out of the service before raising
// its managed "Failed to initialize OculusDeviceConfig." exception. There is
// no service, so the honest string is the reason there is none.
static klj_val klj_OculusDeviceConfig_getError(void *env, void *self,
                                               const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("OculusDeviceConfig.getError() -> \"no device-config service\"");
    return (klj_val){.l = kl_jni_new_string("no device-config service on this device")};
}

// Telephony call-state notifications. There is no telephony here, so registering
// a listener that can never fire is exactly right — Unity only wants to be told
// to duck audio during a call, and no call will ever arrive.
static klj_val klj_UnityPlayer_addPhoneCallListener(void *env, void *self,
                                                    const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// System.nanoTime() — and it must be the *same clock* the Choreographer stamps
// its frameTimeNanos from. The engine subtracts one from the other to get its
// frame delta, so two different monotonic clocks would produce a delta made of
// the offset between them: a garbage frame time that looks like a stall or a
// negative step, with nothing pointing back here.
static klj_val klj_System_nanoTime(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec)};
}

// Wall clock, unlike nanoTime — this one is allowed to be the real date.
static klj_val klj_System_currentTimeMillis(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000)};
}

// Who installed this package. null is Android's answer for "not installed by a
// package installer that recorded itself" — a sideloaded build — which is exactly
// this situation, so it is the true answer rather than a stand-in for one.
static klj_val klj_PackageManager_getInstallerPackageName(void *env, void *self,
                                                          const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// There is no vibrator. False is the honest answer and the useful one: it is the
// documented way to say so, and it keeps Unity from offering haptics it cannot
// deliver. (Quest controllers have haptics, but those arrive through the XR
// runtime's own API, not android.os.Vibrator.)
static klj_val klj_Vibrator_hasVibrator(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// No proxy is configured, and null is how Android says so — the caller is asking
// "what proxy should I use for this URL", and "none" is a real answer rather than
// a missing one.
static klj_val klj_UnityPlayer_getNetworkProxySettings(void *env, void *self,
                                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// Class.getName() returns the *dotted* binary name, not the slashed internal one
// we store — "java.lang.String", not "java/lang/String". Unity round-trips this
// through its reflection helpers and back into FindClass, so getting the
// separator wrong would produce a class name that never matches anything, with
// the failure landing wherever the lookup finally happened.
static klj_val klj_Class_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *internal = klj_class_name(self);
    if (!internal) return (klj_val){.l = NULL};
    char dotted[256];
    size_t i = 0;
    for (; internal[i] && i < sizeof dotted - 1; i++)
        dotted[i] = internal[i] == '/' ? '.' : internal[i];
    dotted[i] = 0;
    return (klj_val){.l = kl_jni_new_string(dotted)};
}

// Locale.getDefault() and its accessors. Answered as a group for the usual
// reason: the language and country have to describe one locale, and Unity reads
// them separately to build Application.systemLanguage.
//
// The value is taken from the host's LANG rather than hardcoded, because unlike
// Build.MODEL this is a user preference and not device identity — the same
// reasoning that makes the synthetic /proc report the host's real core count.
// The fallback is en/US, which is what an unset LANG means in practice.
static void klj_locale_parts(char *lang, size_t lang_sz, char *country, size_t country_sz) {
    snprintf(lang, lang_sz, "en");
    snprintf(country, country_sz, "US");
    const char *env = getenv("LANG");
    if (!env || !*env || strncmp(env, "C", 2) == 0) return;
    // "en_US.UTF-8" -> "en" + "US"
    char buf[64];
    snprintf(buf, sizeof buf, "%s", env);
    char *dot = strchr(buf, '.');
    if (dot) *dot = 0;
    char *us = strchr(buf, '_');
    if (us) {
        *us = 0;
        snprintf(country, country_sz, "%s", us + 1);
    }
    if (*buf) snprintf(lang, lang_sz, "%s", buf);
}

static klj_val klj_Locale_getDefault(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *locale;
    if (!locale) {
        char lang[16], country[16];
        klj_locale_parts(lang, sizeof lang, country, sizeof country);
        locale = kl_jni_new_object("java/util/Locale");
        KLJ_LOG("Locale.getDefault() -> %s_%s (from the host LANG)", lang, country);
    }
    return (klj_val){.l = locale};
}

static klj_val klj_Locale_getLanguage(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char lang[16], country[16];
    klj_locale_parts(lang, sizeof lang, country, sizeof country);
    return (klj_val){.l = kl_jni_new_string(lang)};
}

static klj_val klj_Locale_getCountry(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char lang[16], country[16];
    klj_locale_parts(lang, sizeof lang, country, sizeof country);
    return (klj_val){.l = kl_jni_new_string(country)};
}

// null means "this route is not presenting to a secondary display", which is the
// ordinary case and the one Unity is checking for. Documented as nullable, so this
// is the API's own answer rather than a stub standing in for one.
static klj_val klj_RouteInfo_getPresentationDisplay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// new String(bytes, charsetName). Our jstrings are NUL-terminated byte buffers
// because we define the representation, so for a UTF-8 charset this is a copy
// with a terminator. Any other charset would need a real transcode, so it is
// reported rather than silently mangled.
static klj_val klj_String_init_bytes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_array  *arr     = n > 0 && a[0].l ? klj_arr(a[0].l) : NULL;
    const char *charset = n > 1 ? klj_str(a[1].l) : NULL;
    if (charset && strcasecmp(charset, "UTF-8") && strcasecmp(charset, "UTF8") &&
        strcasecmp(charset, "US-ASCII") && strcasecmp(charset, "ISO-8859-1"))
        KLJ_LOG("new String(byte[], \"%s\") — treated as UTF-8, not transcoded", charset);
    if (!arr || arr->kind != 'B') return (klj_val){.l = kl_jni_new_string("")};
    char *buf = malloc((size_t)arr->len + 1);
    memcpy(buf, arr->data, (size_t)arr->len);
    buf[arr->len] = '\0';
    void *s = kl_jni_new_string(buf);
    free(buf);
    return (klj_val){.l = s};
}

// Uri.encode(s): percent-encode everything outside the unreserved set. The set
// is the Android API's, not a guess — it keeps the RFC 3986 unreserved
// characters plus the sub-delims Uri leaves alone, and a space becomes %20
// rather than '+', which is the difference from form encoding.
static klj_val klj_Uri_encode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    if (!s) return (klj_val){.l = NULL};
    static const char *keep = "_-!.~'()*";
    size_t len = strlen(s);
    char  *out = malloc(len * 3 + 1);
    size_t w   = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || strchr(keep, c)) out[w++] = (char)c;
        else w += (size_t)sprintf(out + w, "%%%02X", c);
    }
    out[w] = '\0';
    void *r = kl_jni_new_string(out);
    free(out);
    return (klj_val){.l = r};
}

// ---- AlertDialog ----
// This is a measurement instrument before it is a UI shim. Unity only reaches
// for a dialog when it has something to say, and the title and message it sets
// are the engine's own diagnosis — far more useful logged than drawn. Nothing
// here renders anything; the builder records and reports.
typedef struct { char *title, *message; } klj_dialog;

static klj_dialog *klj_dlg(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}
static klj_val klj_AlertBuilder_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_as_object(obj)->data = calloc(1, sizeof(klj_dialog));
    KLJ_LOG("AlertDialog.Builder: the guest is raising a dialog");
    return (klj_val){.l = obj};
}
// Every Builder setter returns `this`; that is what the guest's chained calls
// depend on.
#define KLJ_ALERT_SET(Sfx, field)                                                 \
    static klj_val klj_AlertBuilder_set##Sfx(void *env, void *self,               \
                                             const klj_val *a, int n) {           \
        (void)env;                                                                \
        klj_dialog *d = klj_dlg(self);                                            \
        const char *s = n > 0 ? klj_str(a[0].l) : NULL;                           \
        if (d) { free(d->field); d->field = s ? strdup(s) : NULL; }               \
        KLJ_LOG("AlertDialog." #field ": %s", s ? s : "(null)");                  \
        return (klj_val){.l = self};                                              \
    }
KLJ_ALERT_SET(Title,   title)
KLJ_ALERT_SET(Message, message)
#undef KLJ_ALERT_SET

// ---- manifest meta-data ----
// PackageItemInfo.metaData is the <meta-data> block from AndroidManifest.xml,
// and it is real configuration rather than boilerplate: Unity reads its splash
// settings from here and the Oculus layer reads focus-awareness and the
// supported-device list. Returning a null Bundle would silently drop all of it,
// so the values are transcribed from the APK's own manifest.
static const struct { const char *key, *val; } g_metadata[] = {
    {"unity.splash-mode",                   "0"},
    {"unity.splash-enable",                 "false"},
    {"unity.build-id",                      "7a1e012b-c456-4b79-b3d2-b878d039f91e"},
    {"unityplayer.SkipPermissionsDialog",   "false"},
    {"com.oculus.ossplash",                 "true"},
    {"com.oculus.vr.focusaware",            "true"},
    {"com.oculus.supportedDevices",         "quest|delmar"},
    {"com.samsung.android.vr.application.mode", "vr_only"},
    {NULL, NULL},
};

static const char *klj_meta(const char *key) {
    for (int i = 0; g_metadata[i].key; i++)
        if (key && strcmp(g_metadata[i].key, key) == 0) return g_metadata[i].val;
    return NULL;
}

static klj_val klj_metaData_field(void) {
    static void *bundle;
    if (!bundle) bundle = kl_jni_new_object("android/os/Bundle");
    return (klj_val){.l = bundle};
}

// Bundle accessors. The two-argument forms take a default, which is what Android
// returns for a missing key; the one-argument forms return the type's zero.
static klj_val klj_Bundle_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *v = klj_meta(n > 0 ? klj_str(a[0].l) : NULL);
    if (!v && n > 1) return (klj_val){.l = a[1].l};
    return (klj_val){.l = v ? kl_jni_new_string(v) : NULL};
}
static klj_val klj_Bundle_getBoolean(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *v = klj_meta(n > 0 ? klj_str(a[0].l) : NULL);
    if (!v) return (klj_val){.j = n > 1 ? a[1].j : 0};
    return (klj_val){.j = strcmp(v, "true") == 0};
}
static klj_val klj_Bundle_getInt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *v = klj_meta(n > 0 ? klj_str(a[0].l) : NULL);
    if (!v) return (klj_val){.j = n > 1 ? a[1].j : 0};
    return (klj_val){.j = (uint64_t)(int64_t)strtol(v, NULL, 10)};
}
static klj_val klj_Bundle_containsKey(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    return (klj_val){.j = klj_meta(n > 0 ? klj_str(a[0].l) : NULL) != NULL};
}

// PackageInfo, like ApplicationInfo, is read field-by-field. Values come from
// the APK's own manifest (apktool.yml: versionCode 545, versionName 1.28.0_...).
static klj_val klj_PM_getPackageInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *pi;
    return klj_singleton("android/content/pm/PackageInfo", &pi);
}

static klj_val klj_Context_getApplicationInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *ai;
    return klj_singleton("android/content/pm/ApplicationInfo", &ai);
}

static void klj_mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

// java/io/File carries its path as the payload; that is all anything reads.
static void *klj_new_file(const char *path) {
    klj_mkdir_p(path);
    void *obj = kl_jni_new_object("java/io/File");
    klj_as_object(obj)->data = strdup(path);
    return obj;
}
static klj_val klj_File_getPath(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    return (klj_val){.l = kl_jni_new_string(o && o->data ? o->data : "")};
}
// java.io.File.getParent() returns null, not "", when there is no parent.
static klj_val klj_File_getParent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o    = klj_as_object(self);
    const char *path = o ? o->data : NULL;
    const char *cut  = path ? strrchr(path, '/') : NULL;
    if (!cut) return (klj_val){.l = NULL};
    char parent[1024];
    size_t len = (size_t)(cut - path);
    if (len == 0) len = 1;                       // "/x" -> "/"
    if (len >= sizeof parent) len = sizeof parent - 1;
    memcpy(parent, path, len);
    parent[len] = '\0';
    return (klj_val){.l = kl_jni_new_string(parent)};
}

// getExternalFilesDir(type): type selects a subdirectory ("music", "pictures"),
// and null means the root — which is what Unity passes.
static klj_val klj_Context_getExternalFilesDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *type = n > 0 && a[0].l ? klj_str(a[0].l) : NULL;
    char path[1024];
    if (type && *type) snprintf(path, sizeof path, "%s/files/%s", g_files_dir, type);
    else               snprintf(path, sizeof path, "%s/files", g_files_dir);
    KLJ_LOG("getExternalFilesDir(%s) -> %s", type ? type : "null", path);
    return (klj_val){.l = klj_new_file(path)};
}
static klj_val klj_Context_getFilesDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char path[1024];
    snprintf(path, sizeof path, "%s/files", g_files_dir);
    return (klj_val){.l = klj_new_file(path)};
}
static klj_val klj_Context_getCacheDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char path[1024];
    snprintf(path, sizeof path, "%s/cache", g_files_dir);
    return (klj_val){.l = klj_new_file(path)};
}

// Our storage is a plain writable directory, so "mounted" is the honest state.
// Any other value sends Unity down a read-only or unavailable-storage path.
static klj_val klj_Environment_getExternalStorageState(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("mounted")};
}
static klj_val klj_Environment_getExternalStorageDirectory(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_file(g_files_dir)};
}

// There is no ARCore here and there never will be — Vision Pro's world sensing
// arrives through ARKit under our own ovrp_* layer (M6), not through Google AR.
// False is the truthful answer, and Unity has a supported no-AR path.
static klj_val klj_UnityPlayer_initializeGoogleAr(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
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

// ---- SharedPreferences ----
// Unity's PlayerPrefs. This one has to persist: a value written this run must
// read back the next, so it is a real file rather than an in-memory map. The
// file is Android's own shared_prefs/<name>.xml, which costs a small serialiser
// and buys inspectability — and a prefs file pulled off a Quest drops straight
// in. Set<String> is absent because nothing has asked for it; the primitive
// types are all one shape, so they come as a group.
typedef struct { klj_pref *v; unsigned n, cap; } klj_pref_set;

typedef struct {
    char         name[128];
    char         path[1024];
    klj_pref_set kv;
} klj_prefs;

// An Editor batches: Android does not publish a put until commit()/apply(), and
// a read through the SharedPreferences in between still sees the old value.
typedef struct {
    klj_prefs   *store;
    klj_pref_set pending;
    int          clear;
} klj_editor;

static klj_pref *klj_pref_find(klj_pref_set *s, const char *key) {
    for (unsigned i = 0; i < s->n; i++)
        if (strcmp(s->v[i].key, key) == 0) return &s->v[i];
    return NULL;
}

// Find or create, and reset whatever value was there — every caller is about to
// overwrite it.
static klj_pref *klj_pref_slot(klj_pref_set *s, const char *key) {
    klj_pref *p = klj_pref_find(s, key);
    if (p) { free(p->sval); p->sval = NULL; p->ival = 0; p->fval = 0; return p; }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v   = realloc(s->v, s->cap * sizeof *s->v);
    }
    p = &s->v[s->n++];
    memset(p, 0, sizeof *p);
    p->key = strdup(key);
    return p;
}

static void klj_pref_erase(klj_pref_set *s, const char *key) {
    klj_pref *p = klj_pref_find(s, key);
    if (!p) return;
    free(p->key);
    free(p->sval);
    *p = s->v[--s->n];   // order is not part of the API
}

static void klj_pref_clear(klj_pref_set *s) {
    for (unsigned i = 0; i < s->n; i++) { free(s->v[i].key); free(s->v[i].sval); }
    s->n = 0;
}

// Only the five entities Android's XmlSerializer emits — which is exactly the
// set our own reader has to understand, since we write every file we read.
static void klj_xml_escape(FILE *f, const char *s) {
    for (; s && *s; s++) switch (*s) {
    case '&':  fputs("&amp;",  f); break;
    case '<':  fputs("&lt;",   f); break;
    case '>':  fputs("&gt;",   f); break;
    case '"':  fputs("&quot;", f); break;
    case '\'': fputs("&apos;", f); break;
    default:   fputc(*s, f);
    }
}
static char *klj_xml_unescape(const char *s, size_t len) {
    static const char *const ent[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
    static const char        rep[] = {'&', '<', '>', '"', '\''};
    char *out = malloc(len + 1), *w = out;
    for (size_t i = 0; i < len; ) {
        if (s[i] != '&') { *w++ = s[i++]; continue; }
        unsigned k = 0;
        for (; k < sizeof rep; k++) {
            size_t el = strlen(ent[k]);
            if (i + el <= len && strncmp(s + i, ent[k], el) == 0) { *w++ = rep[k]; i += el; break; }
        }
        if (k == sizeof rep) *w++ = s[i++];   // not an entity we emit — take it literally
    }
    *w = '\0';
    return out;
}

static const struct { const char *tag; char kind; } g_pref_tags[] = {
    {"string", 'S'}, {"int", 'I'}, {"long", 'J'}, {"float", 'F'}, {"boolean", 'Z'}, {NULL, 0},
};

static void klj_prefs_save(klj_prefs *p) {
    FILE *f = fopen(p->path, "wb");
    if (!f) { KLJ_LOG("SharedPreferences[%s]: cannot write %s", p->name, p->path); return; }
    fputs("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n", f);
    for (unsigned i = 0; i < p->kv.n; i++) {
        klj_pref *e = &p->kv.v[i];
        if (e->kind == 'S') {
            fputs("    <string name=\"", f);
            klj_xml_escape(f, e->key);
            fputs("\">", f);
            klj_xml_escape(f, e->sval ? e->sval : "");
            fputs("</string>\n", f);
            continue;
        }
        const char *tag = "boolean";
        for (int t = 0; g_pref_tags[t].tag; t++)
            if (g_pref_tags[t].kind == e->kind) { tag = g_pref_tags[t].tag; break; }
        char val[64];
        if (e->kind == 'F')      snprintf(val, sizeof val, "%.9g", (double)e->fval);
        else if (e->kind == 'Z') snprintf(val, sizeof val, "%s", e->ival ? "true" : "false");
        else                     snprintf(val, sizeof val, "%lld", (long long)e->ival);
        fprintf(f, "    <%s name=\"", tag);
        klj_xml_escape(f, e->key);
        fprintf(f, "\" value=\"%s\" />\n", val);
    }
    fputs("</map>\n", f);
    fclose(f);
}

// A reader for exactly the subset above, not an XML parser. Every '<' that is
// not one of our five tags is skipped, so the prolog and <map> need no cases.
static void klj_prefs_load(klj_prefs *p) {
    FILE *f = fopen(p->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return; }
    char  *buf = malloc((size_t)sz + 1);
    size_t len = fread(buf, 1, (size_t)sz, f);
    buf[len] = '\0';
    fclose(f);

    for (char *q = buf; (q = strchr(q, '<')) != NULL; ) {
        q++;
        char kind = 0;
        for (int i = 0; g_pref_tags[i].tag; i++) {
            size_t tl = strlen(g_pref_tags[i].tag);
            if (strncmp(q, g_pref_tags[i].tag, tl) == 0 && (q[tl] == ' ' || q[tl] == '\t')) {
                kind = g_pref_tags[i].kind;
                break;
            }
        }
        if (!kind) continue;
        char *close = strchr(q, '>');
        char *nm    = strstr(q, "name=\"");
        if (!close || !nm || nm > close) continue;
        nm += 6;
        char *ne = strchr(nm, '"');
        if (!ne || ne > close) continue;
        char *key = klj_xml_unescape(nm, (size_t)(ne - nm));

        if (kind == 'S') {
            // <string name="k" /> is how a null value is written; that is
            // indistinguishable from absent to every reader, so drop it.
            char *end = close[-1] == '/' ? NULL : strstr(close + 1, "</string>");
            if (end) {
                char     *val = klj_xml_unescape(close + 1, (size_t)(end - close - 1));
                klj_pref *e   = klj_pref_slot(&p->kv, key);
                e->kind = 'S';
                e->sval = val;
            }
        } else {
            char *vs = strstr(q, "value=\"");
            char *ve = vs && vs < close ? strchr(vs + 7, '"') : NULL;
            if (ve) {
                char val[64];
                size_t vl = (size_t)(ve - vs - 7);
                if (vl >= sizeof val) vl = sizeof val - 1;
                memcpy(val, vs + 7, vl);
                val[vl] = '\0';
                klj_pref *e = klj_pref_slot(&p->kv, key);
                e->kind = kind;
                if (kind == 'F')      e->fval = strtof(val, NULL);
                else if (kind == 'Z') e->ival = strcmp(val, "true") == 0;
                else                  e->ival = strtoll(val, NULL, 10);
            }
        }
        free(key);
        q = close;
    }
    free(buf);
}

// One SharedPreferences object per name, as Android does — callers hold on to
// the reference and expect writes through one to be visible through another.
#define KLJ_MAX_PREF_FILES 8
static klj_prefs g_prefs_files[KLJ_MAX_PREF_FILES];
static void     *g_prefs_objs[KLJ_MAX_PREF_FILES];
static unsigned  g_nprefs_files;

static klj_val klj_Context_getSharedPreferences(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name) name = "default";
    for (unsigned i = 0; i < g_nprefs_files; i++)
        if (strcmp(g_prefs_files[i].name, name) == 0) return (klj_val){.l = g_prefs_objs[i]};
    if (g_nprefs_files == KLJ_MAX_PREF_FILES) {
        KLJ_LOG("SharedPreferences: file table full, refusing \"%s\"", name);
        return (klj_val){.l = NULL};
    }
    klj_prefs *p = &g_prefs_files[g_nprefs_files];
    snprintf(p->name, sizeof p->name, "%s", name);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/shared_prefs", g_files_dir);
    klj_mkdir_p(dir);
    snprintf(p->path, sizeof p->path, "%s/%s.xml", dir, name);
    klj_prefs_load(p);
    // The mode is ignored: MODE_PRIVATE is the only one not deprecated, and
    // there is no second process here to share with.
    KLJ_LOG("getSharedPreferences(\"%s\", %d) -> %s (%u entries)",
            name, n > 1 ? (int)a[1].j : 0, p->path, p->kv.n);
    void *obj = kl_jni_new_object("android/content/SharedPreferences");
    klj_as_object(obj)->data = p;
    g_prefs_objs[g_nprefs_files++] = obj;
    return (klj_val){.l = obj};
}

static klj_prefs *klj_prefs_of(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_pref *klj_prefs_lookup(void *self, const klj_val *a, int n, char kind) {
    klj_prefs  *p   = klj_prefs_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (!p || !key) return NULL;
    klj_pref *e = klj_pref_find(&p->kv, key);
    // Android throws ClassCastException on a type mismatch. Nothing here can
    // throw, so a mismatch reads as absent — the caller gets its own default,
    // which is the closest non-throwing answer. Logged, because it means the
    // file disagrees with the code that wrote it.
    if (e && e->kind != kind) {
        KLJ_LOG("SharedPreferences[%s]: \"%s\" is '%c' but was read as '%c' — using the default",
                p->name, key, e->kind, kind);
        return NULL;
    }
    return e;
}

static klj_val klj_SP_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_pref *e = klj_prefs_lookup(self, a, n, 'S');
    if (!e) return (klj_val){.l = n > 1 ? a[1].l : NULL};
    return (klj_val){.l = kl_jni_new_string(e->sval ? e->sval : "")};
}
#define KLJ_SP_GET(Sfx, kind, Field, Slot)                                        \
    static klj_val klj_SP_get##Sfx(void *env, void *self, const klj_val *a, int n) { \
        (void)env;                                                                \
        klj_pref *e = klj_prefs_lookup(self, a, n, kind);                         \
        if (!e) return (klj_val){.Slot = n > 1 ? a[1].Slot : 0};                  \
        return (klj_val){.Slot = e->Field};                                       \
    }
KLJ_SP_GET(Int,     'I', ival, j)
KLJ_SP_GET(Long,    'J', ival, j)
KLJ_SP_GET(Boolean, 'Z', ival, j)
KLJ_SP_GET(Float,   'F', fval, d)
#undef KLJ_SP_GET

static klj_val klj_SP_contains(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_prefs  *p   = klj_prefs_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = p && key && klj_pref_find(&p->kv, key) != NULL};
}

// getAll() hands back a *copy* in Android, and that is not incidental here: the
// path that calls it is Unity migrating one prefs file into another, so a live
// alias would be read while its source is being written.
static klj_pref_set *klj_pref_snapshot(const klj_pref_set *src) {
    klj_pref_set *s = calloc(1, sizeof *s);
    for (unsigned i = 0; i < src->n; i++) {
        klj_pref *d = klj_pref_slot(s, src->v[i].key);
        d->kind = src->v[i].kind;
        d->ival = src->v[i].ival;
        d->fval = src->v[i].fval;
        d->sval = src->v[i].sval ? strdup(src->v[i].sval) : NULL;
    }
    return s;
}

static klj_val klj_SP_getAll(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_prefs *p = klj_prefs_of(self);
    void      *obj = kl_jni_new_object("java/util/Map");
    klj_as_object(obj)->data = p ? klj_pref_snapshot(&p->kv) : calloc(1, sizeof(klj_pref_set));
    KLJ_LOG("SharedPreferences[%s]: getAll() -> %u entries",
            p ? p->name : "?", p ? p->kv.n : 0);
    return (klj_val){.l = obj};
}

static klj_val klj_SP_edit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor *ed = calloc(1, sizeof *ed);
    ed->store = klj_prefs_of(self);
    void *obj = kl_jni_new_object("android/content/SharedPreferences$Editor");
    klj_as_object(obj)->data = ed;
    return (klj_val){.l = obj};
}

static klj_editor *klj_editor_of(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

// Every Editor mutator returns `this` — that is what makes the chaining Unity
// writes work, not a convenience.
static klj_val klj_editor_put(void *self, const klj_val *a, int n, char kind) {
    klj_editor *ed  = klj_editor_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (!ed || !key) return (klj_val){.l = self};
    klj_pref *e = klj_pref_slot(&ed->pending, key);
    e->kind = kind;
    if (kind == 'S') {
        const char *v = n > 1 ? klj_str(a[1].l) : NULL;
        if (v) e->sval = strdup(v);
        else   e->kind = '-';     // putString(key, null) is documented to remove
    } else if (kind == 'F') {
        e->fval = n > 1 ? (float)a[1].d : 0.0f;
    } else {
        e->ival = n > 1 ? (int64_t)a[1].j : 0;
    }
    return (klj_val){.l = self};
}
#define KLJ_ED_PUT(Sfx, kind)                                                     \
    static klj_val klj_ED_put##Sfx(void *env, void *self, const klj_val *a, int n) { \
        (void)env; return klj_editor_put(self, a, n, kind);                       \
    }
KLJ_ED_PUT(String, 'S') KLJ_ED_PUT(Int,   'I') KLJ_ED_PUT(Long, 'J')
KLJ_ED_PUT(Float,  'F') KLJ_ED_PUT(Boolean, 'Z')
#undef KLJ_ED_PUT

static klj_val klj_ED_remove(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_editor *ed  = klj_editor_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (ed && key) klj_pref_slot(&ed->pending, key)->kind = '-';
    return (klj_val){.l = self};
}
static klj_val klj_ED_clear(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor *ed = klj_editor_of(self);
    if (ed) ed->clear = 1;
    return (klj_val){.l = self};
}

// clear() runs before the puts in the same Editor regardless of call order —
// that ordering is the documented contract, not an implementation detail.
// commit() writes synchronously and apply() does not; with no other process to
// race, both reduce to the same flush.
static void klj_editor_flush(void *self) {
    klj_editor *ed = klj_editor_of(self);
    if (!ed || !ed->store) return;
    klj_prefs *p = ed->store;
    if (ed->clear) klj_pref_clear(&p->kv);
    for (unsigned i = 0; i < ed->pending.n; i++) {
        klj_pref *s = &ed->pending.v[i];
        if (s->kind == '-') { klj_pref_erase(&p->kv, s->key); continue; }
        klj_pref *d = klj_pref_slot(&p->kv, s->key);
        d->kind = s->kind;
        d->ival = s->ival;
        d->fval = s->fval;
        d->sval = s->sval ? strdup(s->sval) : NULL;
    }
    KLJ_LOG("SharedPreferences[%s]: %s%u change%s -> %u entries in %s", p->name,
            ed->clear ? "clear + " : "", ed->pending.n,
            ed->pending.n == 1 ? "" : "s", p->kv.n, p->path);
    klj_prefs_save(p);
    klj_pref_clear(&ed->pending);
    ed->clear = 0;
}
static klj_val klj_ED_commit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor_flush(self);
    return (klj_val){.j = 1};
}
static klj_val klj_ED_apply(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor_flush(self);
    return (klj_val){0};
}

// ---- the collection view getAll() forces ----
// entrySet() -> iterator() -> Map.Entry is the only route out of a Map over JNI,
// so the whole chain follows from getAll() rather than being speculative. It is
// read-only throughout: nothing mutates one of our snapshots, so there is no
// remove() or setValue() here.
typedef struct { klj_pref_set *set; unsigned pos; } klj_iter;

static klj_pref_set *klj_pset(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_val klj_Map_entrySet(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    void *obj = kl_jni_new_object("java/util/Set");
    klj_as_object(obj)->data = klj_pset(self);
    return (klj_val){.l = obj};
}
static klj_val klj_Coll_size(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_pref_set *s = klj_pset(self);
    return (klj_val){.j = s ? s->n : 0};
}
static klj_val klj_Coll_isEmpty(void *env, void *self, const klj_val *a, int n) {
    return (klj_val){.j = klj_Coll_size(env, self, a, n).j == 0};
}
static klj_val klj_Set_iterator(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_iter *it = calloc(1, sizeof *it);
    it->set = klj_pset(self);
    void *obj = kl_jni_new_object("java/util/Iterator");
    klj_as_object(obj)->data = it;
    return (klj_val){.l = obj};
}
static klj_val klj_Iterator_hasNext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_iter   *it = o ? o->data : NULL;
    return (klj_val){.j = it && it->set && it->pos < it->set->n};
}
static klj_val klj_Iterator_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_iter   *it = o ? o->data : NULL;
    if (!it || !it->set || it->pos >= it->set->n) return (klj_val){.l = NULL};
    void *obj = kl_jni_new_object("java/util/Map$Entry");
    klj_as_object(obj)->data = &it->set->v[it->pos++];
    return (klj_val){.l = obj};
}

// Boxed primitives. getValue() returns Object, so the caller's only way back to
// the number is the box's own accessor — and the box's *class* is how it decides
// which one to call, which is what makes IsInstanceOf's name match sufficient.
static void *klj_box(const klj_pref *e) {
    if (!e) return NULL;
    if (e->kind == 'S') return kl_jni_new_string(e->sval ? e->sval : "");
    const char *cls = e->kind == 'I' ? "java/lang/Integer"
                    : e->kind == 'J' ? "java/lang/Long"
                    : e->kind == 'F' ? "java/lang/Float"
                    :                  "java/lang/Boolean";
    klj_pref *copy = malloc(sizeof *copy);
    *copy      = *e;
    copy->key  = NULL;      // the box holds the value, not the mapping
    copy->sval = NULL;
    void *obj = kl_jni_new_object(cls);
    klj_as_object(obj)->data = copy;
    return obj;
}
static klj_val klj_Entry_getKey(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.l = kl_jni_new_string(e && e->key ? e->key : "")};
}
static klj_val klj_Entry_getValue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    return (klj_val){.l = klj_box(o ? o->data : NULL)};
}
// The boxed constructors — new Boolean(z), new Integer(i) and friends. Boxes have
// existed here since SharedPreferences needed them, but only ever built from the
// inside (klj_box); the guest can also build one directly, and it unboxes through
// the same accessors, so it has to land in the same representation.
static klj_val klj_Box_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    const char *cls = klj_class_name(clazz);
    klj_pref   *e   = calloc(1, sizeof *e);
    if (!e || !cls) return (klj_val){.l = NULL};
    if      (strcmp(cls, "java/lang/Integer") == 0) e->kind = 'I';
    else if (strcmp(cls, "java/lang/Long")    == 0) e->kind = 'J';
    else if (strcmp(cls, "java/lang/Float")   == 0) e->kind = 'F';
    else                                            e->kind = 'Z';
    if (e->kind == 'F') e->fval = n > 0 ? (float)a[0].d : 0.0f;
    else                e->ival = n > 0 ? (int64_t)a[0].j : 0;
    void *obj = kl_jni_new_object(cls);
    klj_as_object(obj)->data = e;
    return (klj_val){.l = obj};
}

// int, long and boolean all unbox from the same integral field; only float
// comes out of the FP bank.
static klj_val klj_Box_integral(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.j = e ? (uint64_t)e->ival : 0};
}
static klj_val klj_Box_floatValue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.d = e ? (double)e->fval : 0.0};
}

static const klj_binding g_bindings[] = {
    {"com/unity3d/player/ReflectionHelper", "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", klj_ReflectionHelper_getFieldID},
    {"com/unity3d/player/ReflectionHelper", "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", klj_ReflectionHelper_getMethodID},
    {"com/unity3d/player/ReflectionHelper", "getFieldSignature", "(Ljava/lang/reflect/Field;)Ljava/lang/String;", klj_ReflectionHelper_getFieldSignature},
    {"java/lang/reflect/Field", "getDeclaringClass", "()Ljava/lang/Class;", klj_Field_getDeclaringClass},
    {"java/lang/reflect/Method", "getName", "()Ljava/lang/String;", klj_Method_getName},
    {"java/lang/reflect/Method", "getDeclaringClass", "()Ljava/lang/Class;", klj_Method_getDeclaringClass},
    {"android/view/View", "setSystemUiVisibility", "(I)V", klj_View_setSystemUiVisibility},
    {"android/view/View", "getSystemUiVisibility", "()I", klj_View_getSystemUiVisibility},
    {"android/view/View", "setOnSystemUiVisibilityChangeListener",
     "(Landroid/view/View$OnSystemUiVisibilityChangeListener;)V", klj_View_setSysUiListener},
    {"android/view/Window", "getDecorView", "()Landroid/view/View;", klj_Window_getDecorView},
    {"android/view/Window", "setFlags", "(II)V", klj_Window_setFlags},
    {"android/os/PowerManager", "isSustainedPerformanceModeSupported", "()Z", klj_PowerManager_sustainedPerf},
    {"android/view/InputDevice", "getDeviceIds", "()[I", klj_InputDevice_getDeviceIds},
    {"android/hardware/input/InputManager", "registerInputDeviceListener",
     "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V",
     klj_InputManager_registerListener},
    {"android/media/AudioManager", "getStreamVolume", "(I)I", klj_AudioManager_getStreamVolume},
    {"com/unity3d/player/UnityPlayer", "getLaunchURL", "()Ljava/lang/String;", klj_UnityPlayer_getLaunchURL},
    {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", klj_Integer_parseInt},
    {"android/media/AudioManager", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", klj_AudioManager_getProperty},
    {"android/content/pm/PackageManager", "hasSystemFeature", "(Ljava/lang/String;)Z", klj_PackageManager_hasSystemFeature},
    {"android/content/Context", "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", klj_Context_checkPermission},

    {"javax/net/ssl/TrustManagerFactory", "getDefaultAlgorithm", "()Ljava/lang/String;", klj_TMF_getDefaultAlgorithm},
    {"javax/net/ssl/TrustManagerFactory", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;", klj_TMF_getInstance},
    {"javax/net/ssl/TrustManagerFactory", "init", "(Ljava/security/KeyStore;)V", klj_TMF_init},
    {"javax/net/ssl/TrustManagerFactory", "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;", klj_TMF_getTrustManagers},

    {"java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;", klj_Class_getClassLoader},
    {"java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", klj_Class_forName},
    {"java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", klj_ClassLoader_findLibrary},

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
    {"android/app/Activity",   "runOnUiThread", "(Ljava/lang/Runnable;)V", klj_Activity_runOnUiThread},
    {"android/app/Activity",   "getWindow", "()Landroid/view/Window;",     klj_Activity_getWindow},
    {"android/app/Activity",   "setRequestedOrientation", "(I)V", klj_Activity_setRequestedOrientation},

    // ---- display, window and orientation ----
    {"android/hardware/display/DisplayManager", "getDisplay",
     "(I)Landroid/view/Display;", klj_DisplayManager_getDisplay},
    {"android/hardware/display/DisplayManager", "registerDisplayListener",
     "(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V",
     klj_DisplayManager_registerDisplayListener},
    {"android/hardware/display/DisplayManager", "unregisterDisplayListener",
     "(Landroid/hardware/display/DisplayManager$DisplayListener;)V",
     klj_DisplayManager_unregisterDisplayListener},
    {"android/util/DisplayMetrics", "<init>", "()V", klj_generic_init},
    {"android/view/Display", "getDisplayId",   "()I", klj_Display_getDisplayId},
    {"android/view/Display", "getWidth",       "()I", klj_Display_getWidth},
    {"android/view/Display", "getHeight",      "()I", klj_Display_getHeight},
    {"android/view/Display", "getRotation",    "()I", klj_Display_getRotation},
    {"android/view/Display", "getRefreshRate", "()F", klj_Display_getRefreshRate},
    {"android/view/Display", "getMetrics",     "(Landroid/util/DisplayMetrics;)V", klj_Display_getMetrics},
    {"android/view/Display", "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", klj_Display_getMetrics},
    {"android/view/Display", "getAppVsyncOffsetNanos",        "()J", klj_Display_getAppVsyncOffsetNanos},
    {"android/view/Display", "getPresentationDeadlineNanos",  "()J", klj_Display_getPresentationDeadlineNanos},
    {"android/view/Display", "isWideColorGamut",              "()Z", klj_Display_isWideColorGamut},
    {"android/view/Window",  "getAttributes",
     "()Landroid/view/WindowManager$LayoutParams;", klj_Window_getAttributes},
    {"android/content/res/Resources", "getIdentifier",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", klj_Resources_getIdentifier},
    // getResources is declared on Context and reached through the theme wrapper
    // in between; the guest resolves the id against whichever it named.
    {"android/view/ContextThemeWrapper", "getResources",
     "()Landroid/content/res/Resources;", klj_Context_getResources},
    {"android/content/Context", "getResources",
     "()Landroid/content/res/Resources;", klj_Context_getResources},

    {"android/net/Uri", "encode", "(Ljava/lang/String;)Ljava/lang/String;", klj_Uri_encode},
    {"java/lang/String", "<init>", "([BLjava/lang/String;)V", klj_String_init_bytes},
    {"com/unity3d/player/UnityPlayer", "loadLibrary", "(Ljava/lang/String;)Z", klj_UnityPlayer_loadLibrary},
    {"java/lang/System", "load", "(Ljava/lang/String;)V", klj_System_load},
    {"java/lang/System", "nanoTime", "()J", klj_System_nanoTime},
    {"java/lang/System", "currentTimeMillis", "()J", klj_System_currentTimeMillis},
    // There is no soft keyboard here; Unity calls hide unconditionally while
    // tearing down text input, so silence is correct rather than a stub.
    {"com/unity3d/player/UnityPlayer", "hideSoftInput", "()V", klj_void_noop},
    {"com/unity3d/player/UnityPlayer", "getNetworkProxySettings", "(Ljava/lang/String;)Ljava/lang/String;", klj_UnityPlayer_getNetworkProxySettings},
    {"com/unity3d/player/UnityPlayer", "addPhoneCallListener", "()V", klj_UnityPlayer_addPhoneCallListener},
    {"android/content/Context", "getContentResolver", "()Landroid/content/ContentResolver;", klj_Context_getContentResolver},
    {"android/provider/Settings$Secure", "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", klj_Settings_Secure_getString},
    // Unity's ReflectionHelper spells every reference return as Object, so it asks
    // for getClass with a signature real Java does not declare. Matching is on the
    // exact string, so both spellings are registered; the ()Ljava/lang/Class; one
    // is further down with the rest of the Object methods.
    {"java/lang/Object", "getClass", "()Ljava/lang/Object;", klj_Object_getClass},
    {"java/lang/Class",  "getName",  "()Ljava/lang/String;", klj_Class_getName},
    // Unity builds this signature from Class.getName(), so it is spelled with
    // dots where a real JNI signature would have slashes. Matched as the string
    // the guest actually asks for.
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "init", "(Lcom.unity3d.player.UnityPlayerActivity;)V", klj_OculusDeviceConfig_init},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getCurrentState", "()I", klj_OculusDeviceConfig_getCurrentState},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getError", "()Ljava/lang/String;", klj_OculusDeviceConfig_getError},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "didPrefetchParamName", "(Ljava/lang/String;)Z", klj_OculusDeviceConfig_didPrefetchParamName},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getBoolean", "(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)Z", klj_OculusDeviceConfig_getBoolean},
    {"android/os/Vibrator", "hasVibrator", "()Z", klj_Vibrator_hasVibrator},
    {"android/content/pm/PackageManager", "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", klj_PackageManager_getInstallerPackageName},
    {"android/net/Uri", "decode", "(Ljava/lang/String;)Ljava/lang/String;", klj_Uri_decode},
    {"java/util/Locale", "getDefault",  "()Ljava/util/Locale;",   klj_Locale_getDefault},
    {"java/util/Locale", "getLanguage", "()Ljava/lang/String;",   klj_Locale_getLanguage},
    {"java/util/Locale", "getCountry",  "()Ljava/lang/String;",   klj_Locale_getCountry},
    {"android/media/AudioManager", "getDevices", "(I)[Landroid/media/AudioDeviceInfo;", klj_AudioManager_getDevices},
    {"android/media/MediaRouter", "getSelectedRoute", "(I)Landroid/media/MediaRouter$RouteInfo;", klj_MediaRouter_getSelectedRoute},
    {"android/media/MediaRouter$RouteInfo", "getPresentationDisplay", "()Landroid/view/Display;", klj_RouteInfo_getPresentationDisplay},

    {"android/app/AlertDialog$Builder", "<init>", "(Landroid/content/Context;)V", klj_AlertBuilder_init},
    {"android/app/AlertDialog$Builder", "setTitle",
     "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setTitle},
    {"android/app/AlertDialog$Builder", "setMessage",
     "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setMessage},

    {"android/os/Process", "setThreadPriority", "(II)V", klj_Process_setThreadPriority},
    {"android/os/Process", "setThreadPriority", "(I)V",  klj_Process_setThreadPriority},
    {"android/os/Process", "myTid",             "()I",   klj_Process_myTid},

    {"android/os/Looper",  "getMainLooper", "()Landroid/os/Looper;",  klj_Looper_getMainLooper},
    // A HandlerThread is a thread with a Looper on it. We have one queue and one
    // drain point (kl_jni_drain_ui_tasks) for the main looper — but a
    // HandlerThread gets a real thread of its own, because the guest blocks
    // waiting on it. See the looper section above.
    {"android/os/Handler", "obtainMessage", "(I)Landroid/os/Message;", klj_Handler_obtainMessage},
    {"android/os/Message", "sendToTarget", "()V", klj_Message_sendToTarget},
    {"android/view/Choreographer", "getInstance", "()Landroid/view/Choreographer;", klj_Choreographer_getInstance},
    {"android/view/Choreographer", "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", klj_Choreographer_postFrameCallback},
    {"android/os/HandlerThread", "<init>", "(Ljava/lang/String;)V", klj_HandlerThread_init},
    {"android/os/HandlerThread", "start", "()V", klj_HandlerThread_start},
    {"android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", klj_HandlerThread_getLooper},
    {"java/lang/Thread", "start", "()V", klj_Thread_start},
    {"android/os/Handler", "<init>", "()V",                        klj_Handler_init},
    {"android/os/Handler", "<init>", "(Landroid/os/Looper;)V",     klj_Handler_init},
    // The Callback form. The callback handles Messages sent through this Handler,
    // and nothing here sends Messages — the queue only ever carries Runnables from
    // post/postDelayed — so recording the Looper and dropping the callback is the
    // same single-queue simplification, not a new one. If a Message ever is sent,
    // sendMessage is unimplemented and will say so by name.
    {"android/os/Handler", "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", klj_Handler_init},
    {"android/os/Handler", "post",        "(Ljava/lang/Runnable;)Z",  klj_Handler_post},
    {"android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", klj_Handler_postDelayed},
    {"android/content/Intent", "getExtras",  "()Landroid/os/Bundle;",      klj_Intent_getExtras},
    {"android/content/Intent", "<init>",     "(Ljava/lang/String;)V",      klj_Intent_init},
    {"android/content/Intent", "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", klj_Intent_addCategory},
    {"android/content/Intent", "setPackage",  "(Ljava/lang/String;)Landroid/content/Intent;", klj_Intent_setPackage},
    {"android/content/Intent", "addFlags",    "(I)Landroid/content/Intent;",                 klj_Intent_addFlags},
    {"android/content/Context", "getAssets", "()Landroid/content/res/AssetManager;", klj_Context_getAssets},
    {"android/content/Context", "getPackageManager", "()Landroid/content/pm/PackageManager;", klj_Context_getPackageManager},
    {"android/content/Context", "getPackageName", "()Ljava/lang/String;", klj_Context_getPackageName},
    {"android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", klj_Context_getSystemService},
    {"android/content/Context", "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", klj_Context_getExternalFilesDir},
    {"android/content/Context", "getFilesDir", "()Ljava/io/File;", klj_Context_getFilesDir},
    {"android/content/Context", "getCacheDir", "()Ljava/io/File;", klj_Context_getCacheDir},
    {"android/content/Context", "getPackageCodePath", "()Ljava/lang/String;", klj_Context_getPackageCodePath},
    {"android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", klj_Context_getApplicationInfo},
    {"android/content/Context", "getObbDirs", "()[Ljava/io/File;", klj_Context_getObbDirs},
    {"android/content/Context", "getObbDir",  "()Ljava/io/File;",  klj_Context_getObbDir},
    {"android/content/Context", "getSharedPreferences",
     "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", klj_Context_getSharedPreferences},

    {"android/content/SharedPreferences", "getString",
     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", klj_SP_getString},
    {"android/content/SharedPreferences", "getInt",     "(Ljava/lang/String;I)I", klj_SP_getInt},
    {"android/content/SharedPreferences", "getLong",    "(Ljava/lang/String;J)J", klj_SP_getLong},
    {"android/content/SharedPreferences", "getFloat",   "(Ljava/lang/String;F)F", klj_SP_getFloat},
    {"android/content/SharedPreferences", "getBoolean", "(Ljava/lang/String;Z)Z", klj_SP_getBoolean},
    {"android/content/SharedPreferences", "contains",   "(Ljava/lang/String;)Z",  klj_SP_contains},
    {"android/content/SharedPreferences", "getAll",     "()Ljava/util/Map;",      klj_SP_getAll},
    {"android/content/SharedPreferences", "edit",
     "()Landroid/content/SharedPreferences$Editor;", klj_SP_edit},

#define KLJ_ED(name, args, fn) \
    {"android/content/SharedPreferences$Editor", name, \
     "(" args ")Landroid/content/SharedPreferences$Editor;", fn}
    KLJ_ED("putString",  "Ljava/lang/String;Ljava/lang/String;", klj_ED_putString),
    KLJ_ED("putInt",     "Ljava/lang/String;I",                  klj_ED_putInt),
    KLJ_ED("putLong",    "Ljava/lang/String;J",                  klj_ED_putLong),
    KLJ_ED("putFloat",   "Ljava/lang/String;F",                  klj_ED_putFloat),
    KLJ_ED("putBoolean", "Ljava/lang/String;Z",                  klj_ED_putBoolean),
    KLJ_ED("remove",     "Ljava/lang/String;",                   klj_ED_remove),
    KLJ_ED("clear",      "",                                     klj_ED_clear),
#undef KLJ_ED
    {"android/content/SharedPreferences$Editor", "commit", "()Z", klj_ED_commit},
    {"android/content/SharedPreferences$Editor", "apply",  "()V", klj_ED_apply},

    {"java/util/Map", "entrySet", "()Ljava/util/Set;", klj_Map_entrySet},
    {"java/util/Map", "size",     "()I",               klj_Coll_size},
    {"java/util/Map", "isEmpty",  "()Z",               klj_Coll_isEmpty},
    {"java/util/Set", "iterator", "()Ljava/util/Iterator;", klj_Set_iterator},
    {"java/util/Set", "size",     "()I",               klj_Coll_size},
    {"java/util/Set", "isEmpty",  "()Z",               klj_Coll_isEmpty},
    {"java/util/Iterator", "hasNext", "()Z",                  klj_Iterator_hasNext},
    {"java/util/Iterator", "next",    "()Ljava/lang/Object;", klj_Iterator_next},
    {"java/util/Map$Entry", "getKey",   "()Ljava/lang/Object;", klj_Entry_getKey},
    {"java/util/Map$Entry", "getValue", "()Ljava/lang/Object;", klj_Entry_getValue},
    {"java/lang/Boolean", "<init>", "(Z)V", klj_Box_init},
    {"java/lang/Integer", "<init>", "(I)V", klj_Box_init},
    {"java/lang/Long",    "<init>", "(J)V", klj_Box_init},
    {"java/lang/Float",   "<init>", "(F)V", klj_Box_init},
    {"java/lang/Integer", "intValue",     "()I", klj_Box_integral},
    {"java/lang/Long",    "longValue",    "()J", klj_Box_integral},
    {"java/lang/Boolean", "booleanValue", "()Z", klj_Box_integral},
    {"java/lang/Float",   "floatValue",   "()F", klj_Box_floatValue},
    {"android/content/pm/PackageManager", "getPackageInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", klj_PM_getPackageInfo},
    // Same singleton the Context hands out — there is one application here.
    {"android/content/pm/PackageManager", "getApplicationInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", klj_Context_getApplicationInfo},
    {"android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", klj_Environment_getExternalStorageState},
    {"android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", klj_Environment_getExternalStorageDirectory},
    {"java/io/File", "getAbsolutePath", "()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getPath",         "()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getCanonicalPath","()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getParent",       "()Ljava/lang/String;", klj_File_getParent},
    {"android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", klj_AssetManager_open},
    {"android/content/pm/PackageManager", "queryIntentActivities",
     "(Landroid/content/Intent;I)Ljava/util/List;", klj_PM_queryIntentActivities},
    {"java/lang/Object", "<init>",   "()V",                   klj_generic_init},
    {"java/lang/Object", "getClass", "()Ljava/lang/Class;", klj_Object_getClass},
    {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", klj_String_equals},
    {"java/lang/String", "length", "()I",                   klj_String_length},
    {"com/unity3d/player/UnityPlayer", "initializeGoogleAr", "()Z", klj_UnityPlayer_initializeGoogleAr},

    {"bitter/jnibridge/JNIBridge", "newInterfaceProxy",
     "(J[Ljava/lang/Class;)Ljava/lang/Object;", klj_JNIBridge_newInterfaceProxy},

    {"android/os/Bundle", "getString",  "(Ljava/lang/String;)Ljava/lang/String;", klj_Bundle_getString},
    {"android/os/Bundle", "getString",  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", klj_Bundle_getString},
    {"android/os/Bundle", "getBoolean", "(Ljava/lang/String;)Z",  klj_Bundle_getBoolean},
    {"android/os/Bundle", "getBoolean", "(Ljava/lang/String;Z)Z", klj_Bundle_getBoolean},
    {"android/os/Bundle", "getInt",     "(Ljava/lang/String;)I",  klj_Bundle_getInt},
    {"android/os/Bundle", "getInt",     "(Ljava/lang/String;I)I", klj_Bundle_getInt},
    {"android/os/Bundle", "containsKey","(Ljava/lang/String;)Z",  klj_Bundle_containsKey},

    {"java/util/List", "size",    "()I",                 klj_List_size},
    {"java/util/List", "isEmpty", "()Z",                 klj_List_isEmpty},
    {"java/util/List", "get",  "(I)Ljava/lang/Object;",  klj_List_get},

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
    // The defaults are relative literals; the setters resolve what they are
    // given, so this covers the case where nothing set them.
    g_assets_dir     = klj_abspath(g_assets_dir);
    g_files_dir      = klj_abspath(g_files_dir);
    g_apk_path       = klj_abspath(g_apk_path);
    g_native_lib_dir = klj_abspath(g_native_lib_dir);
    // Raw opens of "<apk>/assets/..." (Unity's split assets) are served from
    // the unpacked tree: g_assets_dir points at "<unpacked>/assets", so the
    // tree root is its parent.
    {
        char root[1024];
        snprintf(root, sizeof root, "%s", g_assets_dir);
        char *slash = strrchr(root, '/');
        if (slash && strcmp(slash, "/assets") == 0) *slash = 0;
        kl_guest_path_map(g_apk_path, root);
    }
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
    ENV(NewGlobalRef,         klj_NewGlobalRef);
    ENV(NewLocalRef,          klj_ref_identity);
    ENV(NewWeakGlobalRef,     klj_ref_identity);
    ENV(DeleteGlobalRef,      klj_DeleteGlobalRef);
    ENV(DeleteLocalRef,       klj_DeleteLocalRef);
    ENV(DeleteWeakGlobalRef,  klj_ref_release);
    ENV(IsSameObject,         klj_IsSameObject);
    ENV(IsInstanceOf,         klj_IsInstanceOf);
    ENV(NewString,            klj_NewString);
    ENV(GetStringChars,       klj_GetStringChars);
    ENV(ReleaseStringChars,   klj_ReleaseStringChars);
    ENV(NewStringUTF,         klj_NewStringUTF);
    ENV(GetStringLength,      klj_GetStringLength);
    ENV(GetStringUTFLength,   klj_GetStringUTFLength);
    ENV(GetStringUTFChars,    klj_GetStringUTFChars);
    ENV(ReleaseStringUTFChars, klj_ReleaseStringUTFChars);
    ENV(RegisterNatives,      klj_RegisterNatives);
    ENV(UnregisterNatives,    klj_UnregisterNatives);
    ENV(GetJavaVM,            klj_GetJavaVM);
    ENV(FromReflectedMethod,  klj_FromReflectedMethod);
    ENV(FromReflectedField,   klj_FromReflectedField);
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
#define ENVCALLA(Name) \
    ENV(Call##Name##MethodA, klj_Call##Name##MethodA); \
    ENV(CallStatic##Name##MethodA, klj_CallStatic##Name##MethodA); \
    ENV(CallNonvirtual##Name##MethodA, klj_CallNonvirtual##Name##MethodA)
    ENVCALLA(Object); ENVCALLA(Boolean); ENVCALLA(Byte);  ENVCALLA(Char);
    ENVCALLA(Short);  ENVCALLA(Int);     ENVCALLA(Long);  ENVCALLA(Float);
    ENVCALLA(Double); ENVCALLA(Void);
#undef ENVCALLA
    ENV(NewObjectV, klj_NewObjectV);
    ENV(NewObjectA, klj_NewObjectA);
    ENV(GetArrayLength,          klj_GetArrayLength);
    ENV(NewObjectArray,          klj_NewObjectArray);
    ENV(GetObjectArrayElement,   klj_GetObjectArrayElement);
    ENV(SetObjectArrayElement,   klj_SetObjectArrayElement);
#define ENVARR(Name) \
    ENV(New##Name##Array, klj_New##Name##Array); \
    ENV(Get##Name##ArrayElements, klj_Get##Name##ArrayElements); \
    ENV(Release##Name##ArrayElements, klj_Release##Name##ArrayElements); \
    ENV(Get##Name##ArrayRegion, klj_Get##Name##ArrayRegion); \
    ENV(Set##Name##ArrayRegion, klj_Set##Name##ArrayRegion)
    ENVARR(Boolean); ENVARR(Byte);  ENVARR(Char);   ENVARR(Short);
    ENVARR(Int);     ENVARR(Long);  ENVARR(Float);  ENVARR(Double);
#undef ENVARR
    ENV(GetStaticObjectField,  klj_GetStaticObjectField);
    ENV(GetObjectField,        klj_GetObjectField);
    ENV(GetStaticIntField,     klj_GetStaticIntField);
    ENV(GetIntField,           klj_GetIntField);
    ENV(GetStaticBooleanField, klj_GetStaticBooleanField);
    ENV(GetBooleanField,       klj_GetBooleanField);
    ENV(GetStaticFloatField,   klj_GetStaticFloatField);
    ENV(GetFloatField,         klj_GetFloatField);
    ENV(SetObjectField,        klj_SetObjectField);
    ENV(SetIntField,           klj_SetIntField);
    ENV(SetLongField,          klj_SetLongField);
    ENV(SetBooleanField,       klj_SetBooleanField);
    ENV(SetFloatField,         klj_SetFloatField);
#undef ENV

#define VM(name, fn) g_vm_vtable[KLJ_VM_##name] = (void *)(fn)
    VM(AttachCurrentThread,         klj_AttachCurrentThread);
    VM(AttachCurrentThreadAsDaemon, klj_AttachCurrentThread);
    VM(DetachCurrentThread,         klj_DetachCurrentThread);
    VM(GetEnv,                      klj_GetEnv);
    VM(DestroyJavaVM,               klj_DestroyJavaVM);
#undef VM
}
