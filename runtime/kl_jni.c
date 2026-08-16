// Synthetic JavaVM / JNIEnv. See kl_jni.h for why this exists at all.
//
// Both tables are built from kl_jni_slots.h, which is generated from the NDK's
// jni.h. The X-macro yields two things from one list: an enum of slot indices
// (so overrides are written by *name* and the compiler resolves the index) and
// one named abort stub per slot. Nothing here hardcodes a slot number, which is
// what keeps this immune to the off-by-one class of bug.
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
#include <zlib.h>        // a split-binary guest reads its OBB, and an OBB is a zip
#define COMMON_DIGEST_FOR_OPENSSL 0
#include <CommonCrypto/CommonDigest.h>   // AVPro's Manager.SHA256 — see below
#include <CommonCrypto/CommonKeyDerivation.h>  // ...and its KeyDerivationPBKDF
#include <CommonCrypto/CommonCryptoError.h>    // kCCSuccess
#include <CommonCrypto/CommonCryptor.h>        // ...and its AES-CBC cryptor
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"   // the default target's userdata key
#include "kl_env.h"
#include "kl_cacerts.h"  // the root anchors behind javax.net.ssl, below
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_jni_slots.h"
#include "kl_jni_int.h"
#include "kl_va.h"

static int g_permissive = 0;
void kl_jni_set_permissive(int on) { g_permissive = on; }

// Table construction is deferred to first use; the host may build objects before
// it ever touches the VM handle, so the public constructors force it too.
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static void klj_init(void);


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
    fprintf(stderr, "[jni] this is a work item — the guest wants it, so implement it.\n");
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




 void *klj_class_object(const char *class_name);

const char KLJ_CLASS_CLASS[]  = "java/lang/Class";
const char KLJ_CLASS_STRING[] = "java/lang/String";

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
static uint64_t g_stat_alloc, g_stat_delete, g_stat_pop_freed, g_stat_freed;

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
static void klj_forget_writes(void *obj);

 void klj_frame_forget(klj_object *o) {
    for (klj_frame *f = t_frame; f; f = f->parent)
        for (unsigned i = 0; i < f->n; i++)
            if (f->objs[i] == o) f->objs[i] = NULL;
}

// Caller holds g_lock. Pinned objects, class objects, and non-objects are
// left alone.
 void klj_retire_object_locked(klj_object *o) {
    if (!o || o->magic != KLJ_OBJ_MAGIC || o->pinned) return;
    if (strcmp(o->cls, KLJ_CLASS_CLASS) == 0) return;
    klj_frame_forget(o);
    o->magic = 0;
    g_retired[g_retired_tail] = (unsigned)(o - g_objects);
    g_retired_tail = (g_retired_tail + 1) % KLJ_MAX_OBJECTS;
    g_retired_n++;
}

 klj_object *klj_alloc_object_locked(const char *cls, void *data) {
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
    } else {
        // The old payload dies here, after the quarantine, not at retire.
        if (o->destroy) { o->destroy(o->data); g_stat_freed++; }
        klj_forget_writes(o);
    }
    *o = (klj_object){KLJ_OBJ_MAGIC, 0, cls, data, NULL};
    g_stat_alloc++;
    klj_frame_record(o);
    return o;
}

 klj_object *klj_as_object(void *p) {
    klj_object *o = p;
    return (o && o->magic == KLJ_OBJ_MAGIC) ? o : NULL;
}

// "This object owns its payload, and here is how to release it." Returns the
// object so it can wrap a constructor call. Only set it where ownership is
// unambiguous: an object with no destructor leaks its payload, which is a cost;
// an object that shares one and claims to own it is a double free.
 void *klj_own(void *obj, void (*destroy)(void *)) {
    klj_object *o = klj_as_object(obj);
    if (o) o->destroy = destroy;
    return obj;
}

static klj_native g_natives[KLJ_MAX_NATIVES];
static unsigned   g_nnatives;

static klj_wanted g_wanted[KLJ_MAX_WANTED];
static unsigned   g_nwanted;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Interning gives every class name one stable address, so a klj_object can hold
// a bare `const char *cls` that stays valid forever, and the "have we seen this
// before" log dedupe falls out of the same lookup.
 klj_class *klj_intern_class_locked(const char *name) {
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
 const char *klj_class_name(void *clazz) {
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
 void *klj_new_object_data(const char *class_name, void *data) {
    pthread_once(&g_init_once, klj_init);
    pthread_mutex_lock(&g_lock);
    klj_class  *c = klj_intern_class_locked(class_name);
    klj_object *o = klj_alloc_object_locked(c->name, data);
    pthread_mutex_unlock(&g_lock);
    return o;
}

 void *klj_class_object(const char *class_name) {
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
    o->destroy = free;
    pthread_mutex_unlock(&g_lock);
    return o;
}

// Unwrap a jstring. Returns NULL for anything that is not one, which the
// callers report rather than dereference.
 const char *klj_str(void *s) {
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

// The low-memory signal. On Android the framework calls Activity.onTrimMemory /
// onLowMemory, which reach libunity through UnityPlayer.nativeLowMemory, and
// Unity fires Application.lowMemory — where a title runs
// Resources.UnloadUnusedAssets() and drops its caches. Nothing here ever called
// it, so every cache a Quest build releases under pressure was held forever, and
// the process could only ever climb.
//
// The class is looked up rather than assumed: nativeLowMemory is registered
// through RegisterNatives like the other 45, and which class declares it is the
// natives table's answer to give. Returns 1 if the guest was told.
int kl_jni_low_memory(void) {
    const klj_native *n = NULL;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_nnatives; i++)
        if (strcmp(g_natives[i].name, "nativeLowMemory") == 0) { n = &g_natives[i]; break; }
    pthread_mutex_unlock(&g_lock);
    if (!n) return 0;
    // A static native is called with its declaring class as the second
    // argument. The guest may keep it, so it is the interned jclass, not NULL.
    ((void (*)(void *, void *))n->fn)(kl_jni_env(), kl_jni_class(n->cls));
    return 1;
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

// The host's own FindClass. A native method receives its declaring class as the
// second argument, and SDL3's nativeSetupJNI keeps that jclass in a global ref
// and routes every later static call through it — so a harness that passes NULL
// there does not merely lose a log line, it hands the guest a class handle that
// can never resolve. See kl_jni.h.
void *kl_jni_class(const char *name) { return klj_FindClass(NULL, name); }

 void *klj_GetObjectClass(void *env, void *obj) {
    (void)env;
    klj_object *o = klj_as_object(obj);
    if (!o) {
        KLJ_LOG("GetObjectClass on an untagged pointer %p — every jobject the guest "
                "holds should have come from us", obj);
        // The pointer says nothing; the caller says everything. A jobject that
        // did not come from us is almost always a return value of OURS that the
        // guest did not expect to be NULL, and the frame above names which one.
        kl_fault_print_frames(stderr, NULL);
        // Scouting: the frame names the CALLER, and the call that follows this
        // one names the OBJECT — libunity's generic "resolve a method on a
        // cached handle" helper does GetObjectClass then GetMethodID, so
        // surviving one more call turns "some jobject is NULL" into a method
        // and a signature. java/lang/Object is the least wrong class to hand
        // back; anything done with it afterwards is invented, hence
        // KL_PERMISSIVE only.
        if (kl_permissive()) {
            KLJ_LOG("KL_PERMISSIVE: answering java/lang/Object so the NEXT call "
                    "names what the guest wanted from it");
            pthread_mutex_lock(&g_lock);
            void *c = klj_intern_class_locked("java/lang/Object")->as_object;
            pthread_mutex_unlock(&g_lock);
            return c;
        }
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
 kl_jint klj_ExceptionCheck(void *env)  { (void)env; return 0; }

static __attribute__((noreturn)) void klj_FatalError(void *env, const char *msg) {
    (void)env;
    fprintf(stderr, "\n[jni] FatalError from guest: %s\n", msg ? msg : "(null)");
    kl_jni_report(stderr);
    kl_egl_report(stderr);
    kl_fatal_prepare(); abort();
}

 kl_jint klj_PushLocalFrame(void *env, kl_jint cap) {
    (void)env; (void)cap;
    klj_frame *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->parent = t_frame;
    t_frame = f;
    return 0;
}
 void *klj_PopLocalFrame(void *env, void *result) {
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
// A reference is the identity function here, so DeleteLocalRef cannot mean
// "drop this handle" — it can only mean "retire this object", and those are
// different whenever the object is reachable from somewhere else. Real JNI
// hands out a NEW local ref for every GetObjectArrayElement / GetObjectField,
// and a careful guest deletes each one; doing that to a shared object kills it
// out from under its container.
//
// SDL3 is such a guest. nativeRunMain sizes argv in one pass and copies it in a
// second, and pass one DeleteLocalRefs each element it measured — so pass two
// read retired strings and every argument arrived EMPTY. argc was right, the
// strings were right, and the app simply saw no options: Steam Link's
// EStreamTransport stayed 0 and it reported "couldn't find a streaming game for
// your account" without opening a socket. The rule held for Beat Saber only
// because Beat Saber never deletes a container-derived ref.
//
// The frame record is the only evidence we have of which refs a frame actually
// created, so that is the test: retire only what this thread's open frames
// recorded. An element read back out of an array was allocated elsewhere, is
// listed in no open frame, and survives. With no frame open at all there is no
// bookkeeping to consult, so the old unconditional retire stands — that is the
// behaviour every Beat Saber measurement was taken against, and making it a
// no-op there would leak the pool instead.
static int klj_frame_holds(const klj_object *o) {
    for (klj_frame *f = t_frame; f; f = f->parent)
        for (unsigned i = 0; i < f->n; i++)
            if (f->objs[i] == o) return 1;
    return 0;
}

static void  klj_DeleteLocalRef(void *env, void *obj) {
    (void)env;
    if (!obj) return;                       // DeleteLocalRef(NULL) is legal
    pthread_mutex_lock(&g_lock);
    klj_object *o = klj_as_object(obj);
    if (o) g_stat_delete++;
    if (!t_frame || !o || klj_frame_holds(o)) klj_retire_object_locked(obj);
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
    // Steam Link's own activity really does extend SDLActivity, and
    // SDL3 hands `SDLActivity.getContext()` — i.e. that subclass — to code that
    // then calls Context methods on it. Without this edge, getFilesDir() and
    // every other inherited Context method would have to be re-declared against
    // the subclass, which is the duplication this table exists to avoid.
    {"com/valvesoftware/steamlink/SteamLink",  "org/libsdl/app/SDLActivity"},
    {"org/libsdl/app/SDLActivity",             "android/app/Activity"},
    // libvrlink_scene's activity IS android.app.NativeActivity — the manifest
    // names the framework class directly rather than a subclass — and it calls
    // getIntent() on it. That is an Activity method, so without this edge every
    // inherited Activity method would need re-declaring against NativeActivity.
    {"android/app/NativeActivity",             "android/app/Activity"},
    // BONELAB's own libSLZQuestNative reaches its Context the way a native
    // plugin with no Context has to: ActivityThread.currentActivityThread()
    // .getApplication(). An Application IS a Context (through ContextWrapper),
    // and this edge is what lets it call getExternalCacheDir() on the result
    // rather than needing every Context method re-declared against it.
    // The dialog's checkbox: the guest resolves setOnCheckedChangeListener
    // against CompoundButton and setText against TextView, not against CheckBox,
    // so the real widget chain is what makes one binding serve both.
    {"android/widget/CheckBox",                "android/widget/CompoundButton"},
    {"android/widget/CompoundButton",          "android/widget/Button"},
    {"android/widget/Button",                  "android/widget/TextView"},
    {"android/widget/TextView",                "android/view/View"},
    {"android/app/Application",                "android/content/ContextWrapper"},
    {"android/app/Activity",                   "android/view/ContextThemeWrapper"},
    {"android/view/ContextThemeWrapper",       "android/content/ContextWrapper"},
    {"android/content/ContextWrapper",         "android/content/Context"},
    // The trust store hands back X509Certificate[] (that is what
    // getAcceptedIssuers is declared to return), but getEncoded() is declared
    // on the BASE class, and libunity resolves it there: the only cert class
    // name in the whole library is "java/security/cert/Certificate". So the
    // binding lives on the base and this edge carries it to the subclass the
    // objects actually are — which is also what makes IsInstanceOf answer
    // correctly for a guest that checks.
    {"java/security/cert/X509Certificate",     "java/security/cert/Certificate"},
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

 kl_jint klj_RegisterNatives(void *env, void *clazz, const kl_jni_method *m, kl_jint n) {
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
// mode the first unknown one stops the run, which is the bring-up loop.
// Permissive mode hands back a synthetic id so a single run
// collects the whole batch of lookups an init path performs.
 void *klj_want(void *clazz, const char *name, const char *sig, char kind) {
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
// Two signatures name the same method when their ARGUMENT lists agree — Java
// has no return-type overloading, so "(...)" is the whole of a method's
// identity within a class and the return type is redundant for dispatch.
//
// This is not a relaxation for its own sake. C# spells an object return
// `Ljava/lang/Object;` whatever the method actually declares, because
// AndroidJavaObject.Call<AndroidJavaObject>(...) builds the signature from its
// GENERIC argument — so managed code and native code ask for the identical
// method with different strings, and a binding table keyed on the full
// signature answers one and refuses the other. VRChat asks for
// getApplicationContext, getCacheDir and TimeZone.getDefault that way; the
// alternative is a second entry per method, forever, discovered one abort at a
// time.
//
// Only the return differs — the argument lists must still match exactly, so an
// overload is never taken for its sibling. And BOTH returns must be reference
// types: `()I` and `()Ljava/lang/Object;` have identical argument lists, and
// matching those would marshal an int back as a jobject. The generic spelling
// this exists for is only ever a reference, so requiring that costs nothing and
// keeps the relaxation to exactly the case it was built for.
//
// Class names inside a signature are compared with '.' equal to '/' for the
// same reason: C# builds a signature out of a .NET type name and leaves the
// DOTS in it, so the guest asks for `(Landroid.content.Context;)V` where the
// table declares `(Landroid/content/Context;)V`. A JNI signature never
// legitimately contains a dot inside a class name — the format is slashes —
// so treating them as one character is unambiguous rather than lenient.
// VRChat asks this way for UnityPermissions.hasUserAuthorizedPermission,
// DateFormat.is24HourFormat, ActivityManager.getMemoryInfo and its own
// Info.setContext; each was a separate abort, and each was the same fact.
static int klj_sig_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x == '.') x = '/';
        if (y == '.') y = '/';
        if (x != y) return 0;
    }
    return 1;
}

static int klj_sig_eq(const char *a, const char *b) {
    size_t la = strlen(a);
    return la == strlen(b) && klj_sig_eq_n(a, b, la);
}

static int klj_same_args(const char *a, const char *b) {
    const char *ea = strchr(a, ')'), *eb = strchr(b, ')');
    if (!ea || !eb) return 0;
    if ((ea[1] != 'L' && ea[1] != '[') || (eb[1] != 'L' && eb[1] != '[')) return 0;
    size_t la = (size_t)(ea - a), lb = (size_t)(eb - b);
    return la == lb && klj_sig_eq_n(a, b, la);
}

static const klj_binding *klj_find_binding(const char *cls, const char *name,
                                           const char *sig) {
    // Exact wins outright across EVERY family table, so the answer does not
    // depend on which file a class landed in; a loose match is only ever the
    // fallback.
    const klj_binding *loose = NULL;
    for (const klj_binding *const *t = klj_binding_tables; *t; t++)
        for (const klj_binding *b = *t; b->cls; b++) {
            if (strcmp(b->cls, cls) != 0 || strcmp(b->name, name) != 0) continue;
            if (klj_sig_eq(b->sig, sig)) return b;
            if (!loose && klj_same_args(b->sig, sig)) loose = b;
        }
    // Named, once per (method, signature). A loose match is the right answer
    // for the generic-return case it exists for and a SILENT wrong answer for
    // anything else — the call succeeds, returns something plausible and lands
    // far away — so every one of them is on the record rather than inferred
    // from a table nobody prints.
    if (loose) {
        static const char *seen[64];
        static unsigned nseen;
        unsigned i = 0;
        for (; i < nseen; i++) if (seen[i] == loose->sig) break;
        if (i == nseen && nseen < 64) {
            seen[nseen++] = loose->sig;
            KLJ_LOG("binding %s.%s matched on ARGUMENTS: guest asked %s, table has %s",
                    cls, name, sig, loose->sig);
        }
    }
    return loose;
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
        // A jmethodID we never issued is nearly always a NULL one the guest
        // cached from a lookup it did not make, and the useful question is who
        // is calling — the id itself carries no information. So name the caller
        // out of the image registry (dladdr cannot see guest images) and say
        // which return kind and receiver it used. Without this the report is a
        // pointer value and an abort in the middle of an otherwise clean run.
        size_t off = 0;
        const char *who = kl_addr_image(__builtin_return_address(0), &off);
        klj_object *rcv = klj_as_object(self);
        KLJ_LOG("Call%s%cMethod with a jmethodID we never issued (%p)",
                want == '?' ? "" : "Static-or-instance ", want == '?' ? ' ' : want, mid);
        KLJ_LOG("  called from %s+0x%zx, receiver %s",
                who ? who : "(host)", off,
                rcv ? rcv->cls : (self ? klj_class_name(self) : "(null)"));
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
        fprintf(stderr, "[jni] this is a work item — add it to the family table in runtime/jni/.\n");
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    return zero;
}

 klj_val klj_call(void *env, void *self, void *mid, kl_va *va, char want) {
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

// ---- the PLAIN varargs forms ----------------------------------------------
// Beat Saber never needed these, and the reason is a property of the guest's
// source language rather than of JNI: Unity is C++, so its inline jni.h
// wrappers `va_start` and call the V forms, and a va_list already arrives as an
// AAPCS64 descriptor by reference. SDL3 is C. It calls
// `(*env)->CallStaticBooleanMethod(env, cls, mid, ...)` directly, so the
// arguments arrive spread across x0-x7/v0-v7 with no descriptor anywhere — the
// ordinary variadic-forwarding problem kl_va_thunks.S exists for.
// This half of the shim is Steam Link's alone: Beat Saber never reaches it.
//
// Each entry point is an asm thunk that spills the register file, materialises
// a kl_va over it and calls the handler below. The handlers are one line each
// because the V forms already take a `kl_va *` — there is no re-marshalling to
// do, only a va_list to construct.
//
// Nonvirtual is deliberately absent: it has no V form to delegate to (see the
// A-form macro below, which is where it does exist), and nothing has called
// one. It stays an abort-by-name, per the rule that a call is an assertion.
#define KLJ_VA_CALL(Name, Ret)                                                          \
    Ret klh_jni_Call##Name##Method(void *env, void *self, void *mid, kl_va *va) {        \
        return klj_Call##Name##MethodV(env, self, mid, va);                              \
    }                                                                                    \
    Ret klh_jni_CallStatic##Name##Method(void *env, void *cls, void *mid, kl_va *va) {   \
        return klj_CallStatic##Name##MethodV(env, cls, mid, va);                          \
    }
KLJ_VA_CALL(Object,  void *)
KLJ_VA_CALL(Boolean, uint8_t)
KLJ_VA_CALL(Byte,    int8_t)
KLJ_VA_CALL(Char,    uint16_t)
KLJ_VA_CALL(Short,   int16_t)
KLJ_VA_CALL(Int,     kl_jint)
KLJ_VA_CALL(Long,    int64_t)
KLJ_VA_CALL(Float,   float)
KLJ_VA_CALL(Double,  double)
KLJ_VA_CALL(Void,    void)
#undef KLJ_VA_CALL

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
 void *klj_NewObjectV(void *env, void *clazz, void *mid, kl_va *va) {
    return klj_call(env, clazz, mid, va, '?').l;
}
// ...and its plain varargs twin, for the same C-vs-C++ reason as the Call family.
void *klh_jni_NewObject(void *env, void *clazz, void *mid, kl_va *va) {
    return klj_NewObjectV(env, clazz, mid, va);
}

// ----------------------------------------------------------------- Java arrays

static size_t klj_prim_size(char kind) {
    switch (kind) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default:            return 0;
    }
}

static void klj_array_free(void *p) {
    klj_array *arr = p;
    if (!arr) return;
    free(arr->data);
    free(arr);
}

 void *klj_new_array(char kind, const char *elem_cls, int len) {
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
    // The element buffer is this array's, and it is the biggest single payload
    // in the pool: a leaked byte[] is its whole length, and the guest allocates
    // them per frame. Object arrays own only the buffer — the jobjects in it
    // belong to the pool and are retired by their own frames.
    return klj_own(obj, klj_array_free);
}

 klj_array *klj_arr(void *a) {
    klj_object *o = klj_as_object(a);
    return o ? o->data : NULL;
}

static kl_jint klj_GetArrayLength(void *env, void *a) {
    (void)env;
    klj_array *arr = klj_arr(a);
    return arr ? arr->len : 0;
}

// A direct ByteBuffer: a java.nio.ByteBuffer whose storage is guest memory the
// guest already owns, so there is nothing to copy in either direction and no
// lifetime for us to manage. The guest allocates, we describe.
//
// This is the *easiest* class of JNI object to serve honestly and the easiest
// to get subtly wrong: the JNI contract is that the address and capacity come
// straight back out, so anything we invent — a copy, a bounds adjustment, a
// rounded capacity — would be a buffer the guest wrote into and something else
// read from a different place.
//
// The record is calloc'd and outlives the object, which is the same
// arrangement klj_new_method and the boxed preferences already have.
static const char KLJ_CLASS_BYTEBUFFER[] = "java/nio/ByteBuffer";

 void *klj_NewDirectByteBuffer(void *env, void *address, int64_t capacity) {
    (void)env;
    // A negative capacity or a NULL address is the guest describing a buffer
    // that cannot exist; JNI says the result is undefined, so we say NULL
    // rather than hand back something that reads as a valid empty buffer.
    if (!address || capacity < 0) return NULL;
    klj_direct_buffer *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->addr = address; b->capacity = capacity;
    return klj_new_object_data(KLJ_CLASS_BYTEBUFFER, b);
}

 klj_direct_buffer *klj_direct(void *buf) {
    klj_object *o = klj_as_object(buf);
    // strcmp, not pointer equality: o->cls is the INTERNED copy in the class
    // table, which is a different address from the literal above.
    return (o && strcmp(o->cls, KLJ_CLASS_BYTEBUFFER) == 0) ? o->data : NULL;
}

static void *klj_GetDirectBufferAddress(void *env, void *buf) {
    (void)env;
    klj_direct_buffer *b = klj_direct(buf);
    return b ? b->addr : NULL;
}

// -1 for anything that is not a direct buffer, which is what JNI specifies and
// is distinguishable from a legitimately empty one.
static int64_t klj_GetDirectBufferCapacity(void *env, void *buf) {
    (void)env;
    klj_direct_buffer *b = klj_direct(buf);
    return b ? b->capacity : -1;
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
// A java/lang/String[], built host-side. SDL3's nativeRunMain takes its argv
// this way — the real SteamLink activity passes the intent's "sArgs" extra
// straight through as the third parameter — so a host driving onCreate needs to
// be able to make one. Built through the same klj_new_array the guest's
// NewObjectArray uses, so there is one array representation, not two.
void *kl_jni_new_string_array(const char *const *items, int n) {
    pthread_once(&g_init_once, klj_init);
    void      *obj = klj_new_array('L', KLJ_CLASS_STRING, n);
    klj_array *arr = klj_arr(obj);
    if (!arr) return obj;
    for (int i = 0; i < n; i++)
        ((void **)arr->data)[i] = kl_jni_new_string(items[i]);
    return obj;
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

static const klj_field g_fields[];

// The device identity is single-sourced ON PURPOSE: Build.MANUFACTURER/MODEL
// read over JNI and ro.product.* read through __system_property_get are the
// same strings, because Steam Link asks both ways and a disagreement between
// the two answers is visible to the guest (see the g_sysprops comment in
// kl_libc.c, which was written after the empty-property crash).
//
// KL_BUILD_<FIELD> overrides one of them, and so it has to be applied HERE —
// at the single source both readers go through — or the override would
// re-introduce exactly the disagreement that mapping was added to remove.
//
// It exists because the identity is load-bearing well beyond Oculus branches.
// libshell's BIsVRHeadset() is literally "<ro.product.manufacturer>
// <ro.product.model>" matched against "Oculus Quest" / "Pico " / "HTC VIVE ",
// so the model we report is what decides whether Steam Link's shell asks its
// host for a VR session or a flat 2D one — and those two sessions are
// different wire protocols on different ports.
// Presenting a Quest 2 stays the default and the settled decision; this is the
// A/B, not a new answer.
static const char *klj_field_sval(const klj_field *f) {
    if (!f->sval
        || (strcmp(f->cls, "android/os/Build") != 0
            && strcmp(f->cls, "android/os/Build$VERSION") != 0))
        return f->sval;
    char env[64];
    snprintf(env, sizeof env, "KL_BUILD_%s", f->name);
    const char *v = getenv(env);
    return v ? v : f->sval;
}

// Interned jstrings for constant object fields, parallel to g_fields so the
// table itself stays const. A static final String read twice is the same object
// in Java, and some callers do compare identity.
#define KLJ_MAX_FIELDS 128
static void *g_field_cache[KLJ_MAX_FIELDS];


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

// Drop every write recorded against an object, called when its slot is
// recycled. Two reasons, and the second is not a leak: the table is keyed on
// the object POINTER, and a pointer is a slot rather than an identity, so
// without this the next occupant of the slot inherits the previous one's field
// values — a Message reading someone else's `what`. It also stops the table
// filling, which is an abort rather than a leak (128 entries).
static void klj_forget_writes(void *obj) {
    for (unsigned i = 0; i < g_nfield_writes; )
        if (g_field_writes[i].obj == obj) g_field_writes[i] = g_field_writes[--g_nfield_writes];
        else i++;
}

static klj_val *klj_find_write(void *obj, void *fid) {
    for (unsigned i = 0; i < g_nfield_writes; i++)
        if (g_field_writes[i].obj == obj && g_field_writes[i].fid == fid)
            return &g_field_writes[i].v;
    return NULL;
}

 void klj_field_store(void *obj, void *fid, klj_val v) {
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
                if (!g_field_cache[idx]) {
                    g_field_cache[idx] = kl_jni_new_string(klj_field_sval(f));
                    // Pinned, because the cache outlives the read. A caller that
                    // does GetStringUTFChars/Release and then DeleteLocalRef —
                    // correct JNI, and what libshell does with ShellWifiInfo's
                    // m_sSSID — would otherwise retire the slot we keep handing
                    // back, and every later read returns the corpse:
                    // "expected a java/lang/String, got an untagged pointer".
                    // Same rule as every other host-held singleton here.
                    klj_as_object(g_field_cache[idx])->pinned = 1;
                }
                return (klj_val){.l = g_field_cache[idx]};
            }
            return (klj_val){.l = kl_jni_new_string(klj_field_sval(f))};
        }
        if (want == 'F' || want == 'D') return (klj_val){.d = f->dval};
        return (klj_val){.j = (uint64_t)f->ival};
    }
    KLJ_LOG("no host value for field %s.%s %s", w->cls, w->name, w->sig);
    if (!g_permissive) {
        fprintf(stderr, "[jni] this is a work item — add it to g_fields.\n");
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
// The long pair. SetLongField has been here since the Handler work and its
// reader was missing, which is the asymmetry that gets found by a guest writing
// a handle into its own Java object and reading it back — VRChat does, 35
// frames in.
static int64_t klj_GetStaticLongField(void *e, void *c, void *f)   { (void)e; return (int64_t)klj_field_value(c, f, 'J').j; }
static int64_t klj_GetLongField(void *e, void *o, void *f)         { (void)e; return (int64_t)klj_field_value(o, f, 'J').j; }

// UE4's copy of Build.VERSION.SDK_INT — see the table entry below for why it
// exists. Reads the table it is in, so there is one number.
static klj_val klj_GameActivity_sdkInt(void) {
    return (klj_val){.j = (uint64_t)kl_jni_build_int("SDK_INT", 29)};
}

// Documented Android platform constants — fixed values, not choices.
#define KLJ_FSTR(c, n, v)     {.cls = c, .name = n, .sig = "Ljava/lang/String;", .sval = v}
#define KLJ_FINT(c, n, v)     {.cls = c, .name = n, .sig = "I", .ival = v}
#define KLJ_FFLT(c, n, v)     {.cls = c, .name = n, .sig = "F", .dval = v}
#define KLJ_FFN(c, n, s, f)   {.cls = c, .name = n, .sig = s, .fn = f}
#define KLJ_CTX_SVC(field, name) KLJ_FSTR("android/content/Context", field, name)



static const klj_field g_fields[] = {
    // Audio. The PROPERTY_* values are the real Android key strings, so the
    // getProperty implementation can match on them rather than on our own names.
    KLJ_FSTR("android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE",
             "android.media.property.OUTPUT_SAMPLE_RATE"),
    KLJ_FSTR("android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER",
             "android.media.property.OUTPUT_FRAMES_PER_BUFFER"),
    // DialogInterface's button ids, which a listener switches on to tell
    // Continue from Abort. Negative by Android's definition, not by ours.
    KLJ_FINT("android/content/DialogInterface", "BUTTON_POSITIVE", -1),
    KLJ_FINT("android/content/DialogInterface", "BUTTON_NEGATIVE", -2),
    KLJ_FINT("android/content/DialogInterface", "BUTTON_NEUTRAL",  -3),
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
    // VRChat watches the battery. ACTION_BATTERY_CHANGED is a STICKY broadcast,
    // so the guest's next move is registerReceiver(...) — which on Android
    // returns the last Intent rather than null, and whose extras are the battery
    // state. When that arrives it must read the kl_ovrp battery seam, the same
    // source BatteryManager.isCharging/getIntProperty already answer from: two
    // sources for one battery is a level that disagrees with its own charging
    // flag, and nothing would report it.
    KLJ_FSTR("android/content/Intent", "ACTION_BATTERY_CHANGED",
             "android.intent.action.BATTERY_CHANGED"),

    KLJ_FSTR("android/os/Environment", "MEDIA_MOUNTED", "mounted"),
    KLJ_FFN("android/net/Uri", "EMPTY", "Landroid/net/Uri;", klj_Uri_EMPTY),

    // ApplicationInfo is read field-by-field, and these depend on runtime
    // configuration rather than being compile-time constants.
    KLJ_FFN("android/content/pm/ApplicationInfo", "sourceDir",        "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "publicSourceDir",  "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "nativeLibraryDir", "Ljava/lang/String;", klj_appinfo_nativeLibraryDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "dataDir",          "Ljava/lang/String;", klj_appinfo_dataDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitSourceDirs",       "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitPublicSourceDirs", "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FINT("android/content/pm/ApplicationInfo", "flags", 0),

    // android.hardware.Sensor's type constants. Android's own numbers, from the
    // platform API — nothing here is chosen, and they are only meaningful as
    // arguments to SensorManager.getDefaultSensor, which answers null for every
    // one of them (below).
    //
    // The whole family goes in rather than the one that was asked for, because
    // the guest enumerates: Unity's sensor setup probes a series of types and
    // keeps whichever come back non-null, so stopping at the first is one abort
    // per rerun for no new information.
    KLJ_FINT("android/hardware/Sensor", "TYPE_ACCELEROMETER",          1),
    KLJ_FINT("android/hardware/Sensor", "TYPE_MAGNETIC_FIELD",         2),
    KLJ_FINT("android/hardware/Sensor", "TYPE_GYROSCOPE",              4),
    KLJ_FINT("android/hardware/Sensor", "TYPE_LIGHT",                  5),
    KLJ_FINT("android/hardware/Sensor", "TYPE_PRESSURE",               6),
    KLJ_FINT("android/hardware/Sensor", "TYPE_PROXIMITY",              8),
    KLJ_FINT("android/hardware/Sensor", "TYPE_GRAVITY",                9),
    KLJ_FINT("android/hardware/Sensor", "TYPE_LINEAR_ACCELERATION",   10),
    KLJ_FINT("android/hardware/Sensor", "TYPE_ROTATION_VECTOR",       11),
    KLJ_FINT("android/hardware/Sensor", "TYPE_RELATIVE_HUMIDITY",     12),
    KLJ_FINT("android/hardware/Sensor", "TYPE_AMBIENT_TEMPERATURE",   13),
    KLJ_FINT("android/hardware/Sensor", "TYPE_GAME_ROTATION_VECTOR",  15),
    KLJ_FINT("android/hardware/Sensor", "TYPE_SIGNIFICANT_MOTION",    17),
    KLJ_FINT("android/hardware/Sensor", "TYPE_STEP_DETECTOR",         18),
    KLJ_FINT("android/hardware/Sensor", "TYPE_STEP_COUNTER",          19),
    KLJ_FINT("android/hardware/Sensor", "TYPE_HEART_RATE",            21),

    // Both read from the unpacked tree's apktool.yml — see klj_guest_version.
    // A split-binary guest builds its OBB FILENAME from the version code, so a
    // constant here goes stale into a missing-game-data error on every swap.
    KLJ_FFN("android/content/pm/PackageInfo", "versionName", "Ljava/lang/String;",
            klj_PackageInfo_versionName),
    KLJ_FFN("android/content/pm/PackageInfo", "versionCode", "I",
            klj_PackageInfo_versionCode),
    // ...and so is the package name, out of AndroidManifest.xml, for the same
    // reason: it is the other half of the OBB's filename, and it is how the
    // guest looks itself up. See klj_guest_package.
    KLJ_FFN("android/content/pm/PackageInfo", "packageName", "Ljava/lang/String;",
            klj_PackageInfo_packageName),

    // Unity's own static handle on the Activity. Must be the *same* object the
    // Context was, not another instance of the class — Unity passes one to native
    // code and reads the other back, then compares them.
    //
    // Both spellings of ONE field, because a binding is matched on the full
    // (class, name, signature) and two callers ask with different types. The
    // declared type is Landroid/app/Activity; (UnityPlayer.smali:35) and that is
    // what libOculusXRPlugin's JNI_OnLoad asks for in Beat Saber 1.40; libunity
    // asks with the erased Ljava/lang/Object;, which is what this table carried
    // alone until 1.40 stopped at "no host value for field". Same function, so
    // they cannot become two different activities.
    KLJ_FFN("com/unity3d/player/UnityPlayer", "currentActivity", "Landroid/app/Activity;", klj_currentActivity_field),
    KLJ_FFN("com/unity3d/player/UnityPlayer", "currentActivity", "Ljava/lang/Object;", klj_currentActivity_field),
    // ...and currentContext, which UnityPlayer's constructor sets from the SAME
    // parameter it passes to initJni (UnityPlayer.smali:540, `sput-object p1`).
    // So it must be the same object, not a fresh Context: the guest reaches its
    // Activity through either name and compares what it gets. Two signatures
    // for one field again — the declared type and libunity's erased one.
    KLJ_FFN("com/unity3d/player/UnityPlayer", "currentContext", "Landroid/content/Context;", klj_currentActivity_field),
    KLJ_FFN("com/unity3d/player/UnityPlayer", "currentContext", "Ljava/lang/Object;", klj_currentActivity_field),
    KLJ_FFN("android/graphics/PorterDuff$Mode", "CLEAR", "Landroid/graphics/PorterDuff$Mode;", klj_porterduff_clear),

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
    // UE4's GameActivity copies SDK_INT into a static of its own in its class
    // initializer and then branches on THAT everywhere (13 sites in RE4's
    // GameActivity alone). A guest whose Java we stand in for never runs that
    // initializer, so the copy has to exist here — and it reads the same field
    // rather than restating 29, because two numbers describing one device are
    // the display-panel group answer again.
    KLJ_FFN("com/epicgames/ue4/GameActivity", "ANDROID_BUILD_VERSION", "I",
            klj_GameActivity_sdkInt),
    KLJ_FSTR("android/os/Build$VERSION", "RELEASE", "10"),
    // The build-number half of the version. Matches Build.ID so the two halves of
    // the fingerprint describe one build.
    KLJ_FSTR("android/os/Build$VERSION", "INCREMENTAL", "SQ3A.220605.009.A1"),
    KLJ_FSTR("android/os/Build$VERSION", "CODENAME",    "REL"),
    KLJ_FSTR("android/os/Build", "MANUFACTURER", "Oculus"),
    KLJ_FSTR("android/os/Build", "BRAND",        "oculus"),
    KLJ_FSTR("android/os/Build", "MODEL",        "Quest 2"),
    // The device codename, and it is "hollywood" rather than the "delmar" this
    // used to say. "delmar" came from Beat Saber's own manifest
    // (com.oculus.supportedDevices = quest|delmar), which was the only evidence
    // available at the time — but that is the Oculus STORE's device token, a
    // different namespace from ro.product.name/ro.product.device. Steam Link
    // settles it from the other side: it ships a table of per-headset properties
    // keyed by codename (assets/config/hmd_config.json — hollywood, seacliff,
    // stinson, eureka, panther, kona) and looks its own device up in it by
    // Build.PRODUCT. Those are ro.product.name values, and the Quest 2's is
    // hollywood; with "delmar" the lookup missed and the client told the Steam
    // host it was an unrecognised headset ("[DeviceHMD] Unable to find device
    // static props for ..."). Two Quest titles agreeing beats one manifest.
    KLJ_FSTR("android/os/Build", "DEVICE",       "hollywood"),
    KLJ_FSTR("android/os/Build", "PRODUCT",      "hollywood"),
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
             "oculus/hollywood/hollywood:10/SQ3A.220605.009.A1/1:user/release-keys"),


    KLJ_FINT("android/content/Context", "MODE_PRIVATE", 0),
    KLJ_FINT("android/content/Context", "BIND_AUTO_CREATE", 0),

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

    // The transport constants NetworkInfo.getType() is compared against. The
    // guest reads them as fields rather than hardcoding them, so answering
    // getType() without these leaves it comparing our number to nothing —
    // which stops the run by name (it did). Values from android-34's
    // android.jar; TYPE_WIFI is what we answer, for the reason at
    // klj_NetworkInfo_getType.
    KLJ_FINT("android/net/ConnectivityManager", "TYPE_MOBILE",   0),
    KLJ_FINT("android/net/ConnectivityManager", "TYPE_WIFI",     1),
    KLJ_FINT("android/net/ConnectivityManager", "TYPE_ETHERNET", 9),

    // ...and the same thing for the API that replaced it. A Unity 2022 title
    // asks NetworkCapabilities instead, and reads these as fields for exactly
    // the reason above — VRChat wants NET_CAPABILITY_NOT_METERED, i.e. "may I
    // download freely on this link". Platform constants, so they are facts
    // rather than answers; what we DO with them is klj_NetworkCapabilities_*.
    KLJ_FINT("android/net/NetworkCapabilities", "TRANSPORT_CELLULAR",  KLJ_NC_TRANSPORT_CELLULAR),
    KLJ_FINT("android/net/NetworkCapabilities", "TRANSPORT_WIFI",      KLJ_NC_TRANSPORT_WIFI),
    KLJ_FINT("android/net/NetworkCapabilities", "TRANSPORT_BLUETOOTH", KLJ_NC_TRANSPORT_BLUETOOTH),
    KLJ_FINT("android/net/NetworkCapabilities", "TRANSPORT_ETHERNET",  KLJ_NC_TRANSPORT_ETHERNET),
    KLJ_FINT("android/net/NetworkCapabilities", "TRANSPORT_VPN",       KLJ_NC_TRANSPORT_VPN),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_METERED",   KLJ_NC_CAP_NOT_METERED),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_INTERNET",      KLJ_NC_CAP_INTERNET),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_RESTRICTED",KLJ_NC_CAP_NOT_RESTRICTED),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_TRUSTED",       KLJ_NC_CAP_TRUSTED),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_VPN",       KLJ_NC_CAP_NOT_VPN),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_VALIDATED",     KLJ_NC_CAP_VALIDATED),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_ROAMING",   KLJ_NC_CAP_NOT_ROAMING),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_CONGESTED", KLJ_NC_CAP_NOT_CONGESTED),
    KLJ_FINT("android/net/NetworkCapabilities", "NET_CAPABILITY_NOT_SUSPENDED", KLJ_NC_CAP_NOT_SUSPENDED),

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

    // Steam Link's ShellWifiInfo, read field-by-field off the object that
    // getWifiInfo() returns. These four values are not invented: they are
    // exactly what the app's own ShellWifiInfo.Update() writes when
    // WifiManager.getConnectionInfo() returns null — i.e. Android's answer on a
    // device that is not associated with a Wi-Fi network, which is the truth
    // here (the host reaches the LAN however it reaches it; we model no
    // WifiManager). Strength is calculateSignalLevel(-127, 3) = 0, the same
    // constant that path passes.
    //
    // Claiming a network would be the invented-positive kind of answer: the
    // shell shows link quality from these and would report a signal we have not
    // measured. "Not on Wi-Fi" is also the strictly-conservative direction —
    // Steam Link's own advice for a wired host is that it is the better case.
    KLJ_FINT("com/valvesoftware/steamlink/SteamLink$ShellWifiInfo", "m_nNetworkID", -1),
    KLJ_FINT("com/valvesoftware/steamlink/SteamLink$ShellWifiInfo", "m_nFrequency", -1),
    KLJ_FINT("com/valvesoftware/steamlink/SteamLink$ShellWifiInfo", "m_nStrength",   0),
    KLJ_FSTR("com/valvesoftware/steamlink/SteamLink$ShellWifiInfo", "m_sSSID",      ""),
    {.cls = NULL},
};

// The Build.* String constants, readable from outside the JNI surface. See the
// declaration in kl_jni.h for why this exists rather than a second table in
// kl_libc.c: __system_property_get is the OTHER way to ask the same question,
// and Steam Link asks both ways.
const char *kl_jni_build_string(const char *field) {
    for (const klj_field *f = g_fields; f->cls; f++)
        if (f->sval && strcmp(f->name, field) == 0
            && (strcmp(f->cls, "android/os/Build") == 0
                || strcmp(f->cls, "android/os/Build$VERSION") == 0))
            return klj_field_sval(f);
    return NULL;
}

// ...and the same lookup for an INT Build field. `ro.build.version.sdk` is the
// one property whose Build twin is not a string, and reading it from SDK_INT
// rather than from a second literal is what stops the two from disagreeing —
// which is not hypothetical: they are the same question asked over JNI and over
// __system_property_get, and a guest that asks both and gets 29 and 0 will
// believe the 0.
int kl_jni_build_int(const char *field, int dflt) {
    for (const klj_field *f = g_fields; f->cls; f++)
        if (!f->sval && strcmp(f->name, field) == 0
            && (strcmp(f->cls, "android/os/Build") == 0
                || strcmp(f->cls, "android/os/Build$VERSION") == 0))
            return (int)f->ival;
    return dflt;
}

// The binding tables, one per family file (runtime/jni/kl_jni_*.c).
// klj_find_binding walks them in order; a name absent from all of them
// aborts by name, which is what makes the JNI surface measurable.
const klj_binding *const klj_binding_tables[] = {
    klj_bind_lang,
    klj_bind_android,
    klj_bind_looper,
    klj_bind_display,
    klj_bind_bridge,
    klj_bind_window,
    klj_bind_net,
    klj_bind_softinput,
    klj_bind_services,
    klj_bind_io,
    klj_bind_prefs,
    klj_bind_sdl,
    klj_bind_ue4,
    NULL,
};

// ---------------------------------------------------------------- JavaVM impl
static void *g_env_vtable[KL_JNI_SLOTS_COUNT];
static void *g_vm_vtable[KL_JVM_SLOTS_COUNT];

// A JNIEnv is per-thread by contract, and a thread running guest code needs the
// bionic stack canary in TSD slot 5 before it executes anything, so the
// attach path is also where kl_thread_init() belongs.
typedef struct { void *functions; } klj_env_t;
static pthread_key_t  g_env_key;
static void klj_build_tables(void);
static void klj_init(void) {
    pthread_key_create(&g_env_key, free);
    // The defaults are relative literals; the setters resolve what they are
    // given, so this covers the case where nothing set them.
    g_assets_dir     = klj_abspath(g_assets_dir);
    (void)kl_jni_files_dir();   // resolve now, so the report below prints it
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
    // "/sdcard" and its aliases resolve to the same directory
    // Environment.getExternalStorageDirectory() answers with, which is what
    // stops a guest that asks Java and a guest that hardcodes the path from
    // keeping two user trees. Registered HERE, beside that mapping and from the
    // same source, so neither can be changed without the other.
    kl_guest_extstorage_map(kl_jni_files_dir());
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
// The asm thunks from kl_va_thunks.S. Declared argument-less on purpose: each
// really is variadic with a different return type, and the table stores them as
// void * anyway — a prototype here would be a second, drifting statement of an
// ABI that the thunk and the handler already agree on.
#define KLJ_VA_DECL(Name) \
    extern void klv_jni_Call##Name##Method(void); \
    extern void klv_jni_CallStatic##Name##Method(void)
KLJ_VA_DECL(Object);  KLJ_VA_DECL(Boolean); KLJ_VA_DECL(Byte);  KLJ_VA_DECL(Char);
KLJ_VA_DECL(Short);   KLJ_VA_DECL(Int);     KLJ_VA_DECL(Long);  KLJ_VA_DECL(Float);
KLJ_VA_DECL(Double);  KLJ_VA_DECL(Void);
#undef KLJ_VA_DECL
extern void klv_jni_NewObject(void);

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

    // The V forms are what a C++ guest reaches: its inline jni.h wrappers
    // va_start and call these. A C guest calls the PLAIN varargs form instead,
    // where the arguments arrive in registers with no descriptor — so those get
    // an asm thunk each (kl_va_thunks.S) that materialises a kl_va and hands it
    // to the same implementation. Unity needed only the first column; SDL3
    // needed the second, which is the trace forcing them rather than a guess.
#define ENVCALL(Name) \
    ENV(Call##Name##MethodV, klj_Call##Name##MethodV); \
    ENV(CallStatic##Name##MethodV, klj_CallStatic##Name##MethodV); \
    ENV(Call##Name##Method, klv_jni_Call##Name##Method); \
    ENV(CallStatic##Name##Method, klv_jni_CallStatic##Name##Method)
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
    ENV(NewObject,  klv_jni_NewObject);
    ENV(NewObjectV, klj_NewObjectV);
    ENV(NewObjectA, klj_NewObjectA);
    ENV(GetArrayLength,          klj_GetArrayLength);
    ENV(NewDirectByteBuffer,     klj_NewDirectByteBuffer);
    ENV(GetDirectBufferAddress,  klj_GetDirectBufferAddress);
    ENV(GetDirectBufferCapacity, klj_GetDirectBufferCapacity);
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
    ENV(GetStaticLongField,    klj_GetStaticLongField);
    ENV(GetLongField,          klj_GetLongField);
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
