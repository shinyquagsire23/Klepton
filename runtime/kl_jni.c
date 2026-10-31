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
#include "kl_key.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "guest/kl_driver.h"   // the target name, for per-target JNI defaults
#include "kl_target.h"   // the default target's userdata key
#include "kl_driver.h"   // kl_driver_target_name() — per-target Build identity
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

// KL_JNI_SOFT_UNKNOWN: a Java method or field with no answer here answers zero
// (no-op for void, 0 / false, a null object) and is named in the log, instead
// of ending the run. Only the method and field lookups: unlike KL_PERMISSIVE it
// leaves EGL and GL alone, where a zero is a wrong answer rather than an absent
// feature. Most of these calls are SDKs the game can live without (analytics,
// crash reporting, voice chat, a vendor's own helper class) — about twenty of
// the first 52 fixes users sent in were exactly that. Default on under the
// profile for untested games (kl_untested_profile), off for every other title:
// for a title being brought up, the stop by name is the work item.
static int klj_soft_unknown(void) {
    static int v = -1;
    if (v < 0) {
        extern int kl_untested_profile(void);
        v = kl_env_on("KL_JNI_SOFT_UNKNOWN", kl_untested_profile());
    }
    return v;
}

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
// Frameless strings retired by age — see klj_loose_note.
#define KLJ_LOOSE_MAX 16384
static unsigned g_loose_slot[KLJ_LOOSE_MAX];
static void    *g_loose_data[KLJ_LOOSE_MAX];
static unsigned g_loose_head, g_loose_n;
static int      g_loose_on;
static uint64_t g_stat_loose;

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

// "The HOST is keeping this one in a static, and will hand the same pointer
// back on every later call." That is a lifetime claim, and it has to be made
// explicit or the object dies under the cache.
//
// A binding that caches its answer is doing the right thing — object identity
// is load-bearing for anything Unity keys a slot on (see the InputDevice
// comments) — but the object was still allocated inside whatever local frame
// the FIRST caller happened to have open. PopLocalFrame retires everything that
// frame recorded, `pinned` excepted; the static keeps pointing at the retired
// slot, and the NEXT caller is handed a pointer whose magic is zero. It is not
// a use-after-free — the slot is only quarantined, so the memory still reads
// fine — it is a jobject the guest can hold and we can no longer recognise, and
// it surfaces far away as "GetObjectClass on an untagged pointer".
//
// That is exactly how Paint the Town Red VR's boot died.
// `InputDevice.getDeviceIds()` cached its `int[]` in a static; libunity called
// it three times inside frames that were then popped; Rewired called it a
// fourth time from C#, got the dead pointer back, and asked GetObjectClass for
// its class. The exit census is the proof: `android/view/InputDevice x2` was
// live — getDevice() pins — and there was no live `[I` at all.
//
// So: every host-side static that holds a jobject goes through here. It is the
// same fact klj_singleton already asserts for the objects it makes; this is the
// form for an object the caller built itself — an array, an object with a
// payload, or an element stored inside a cached array (which the array does not
// own: klj_new_array's destructor frees the element BUFFER, and the jobjects in
// it are retired by their own frames unless they are pinned too).
 void *klj_host_cache(void *obj) {
    klj_object *o = klj_as_object(obj);
    if (o) o->pinned++;
    return obj;
}

static klj_native g_natives[KLJ_MAX_NATIVES];
static unsigned   g_nnatives;

static klj_wanted g_wanted[KLJ_MAX_WANTED];
static unsigned   g_nwanted;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Unreal Engine 5 renamed Epic's Java package: `com.epicgames.ue4.*` became
// `com.epicgames.unreal.*` — GameActivity, MediaPlayer14 and its three track-
// info inner classes, the VolumeReceiver/BatteryReceiver/HeadsetReceiver trio,
// MessageBox01. It is a RENAME and not a new API: the method names, signatures
// and semantics the four kl_jni_*.c tables transcribe are identical either side
// of it (verified against Pinball FX VR's UE5.3.2 GameActivity.smali against
// Deadpool VR's 4.27 one).
//
// So it is folded HERE, at the one funnel every class name enters the surface
// through — klj_FindClass, kl_jni_class and kl_jni_new_object all call this —
// rather than by duplicating some sixty table rows under a second spelling. A
// duplicated table is a table that goes stale in one half, and the half that
// goes stale is the one no target exercises yet.
//
// The fold is one-directional and narrow on purpose: only this exact package
// prefix, only onto ue4's. Anything else keeps the name it was asked for, and
// the interned name is the FOLDED one, so klj_class_name, the g_supers walk and
// every klj_object's `cls` pointer all agree on one spelling downstream.
#define KLJ_UE5_PKG "com/epicgames/unreal/"
#define KLJ_UE4_PKG "com/epicgames/ue4/"

static const char *klj_fold_class_name(const char *name, char *buf, size_t n) {
    if (strncmp(name, KLJ_UE5_PKG, sizeof KLJ_UE5_PKG - 1) != 0) return name;
    snprintf(buf, n, KLJ_UE4_PKG "%s", name + sizeof KLJ_UE5_PKG - 1);
    return buf;
}

// Interning gives every class name one stable address, so a klj_object can hold
// a bare `const char *cls` that stays valid forever, and the "have we seen this
// before" log dedupe falls out of the same lookup.
 klj_class *klj_intern_class_locked(const char *asked) {
    char fold[256];
    const char *name = klj_fold_class_name(asked, fold, sizeof fold);
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

// Pin a host-made object so it is never retired at a local-frame pop — the C
// equivalent of the guest calling NewGlobalRef. For objects the host hands back
// and KEEPS a pointer to (a cached SharedPreferences stored in a global table):
// without this the object dies at the next PopLocalFrame and the stored pointer
// comes back as "GetObjectClass on an untagged pointer" when the guest reuses it.
void kl_jni_pin_object(void *obj) {
    pthread_mutex_lock(&g_lock);
    klj_object *o = klj_as_object(obj);
    if (o) o->pinned++;
    pthread_mutex_unlock(&g_lock);
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

// The registered signature for cls.name, or NULL if never registered. The
// bring-up diagnostic behind it: when a signature-specific lookup misses, the
// caller prints this verbatim so the log shows the exact bytes RegisterNatives
// stored instead of leaving two indistinguishable failure modes.
const char *kl_jni_native_sig(const char *cls, const char *name) {
    for (unsigned i = 0; i < g_nnatives; i++)
        if (strcmp(g_natives[i].cls, cls) == 0 && strcmp(g_natives[i].name, name) == 0)
            return g_natives[i].sig;
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
//
// Unity 6.2 registers no nativeLowMemory. Its player has
// nativeMemoryUsageChanged(int) instead, an INSTANCE native taking
// ApplicationMemoryUsage, where Critical (4) raises both
// Application.memoryUsageChanged and the old Application.lowMemory — as the
// sender of fix 38 read it out of BattleGroupVR2 0.251 (2026-10-13); nobody
// here has that build. It is called only where nativeLowMemory is not
// registered, with the one player the lifecycle runs on, and only once that
// player exists: this arrives on the OS's memory-pressure thread too.
//
// KL_LOWMEM_USAGE_CHANGED, default on for that title alone. A Unity 6.2 game
// that runs today is told nothing under pressure and would start to be; what
// each does with it has not been looked at.
static int klj_lowmem_usage_changed(void) {
    const char *t = kl_driver_target_name();
    return kl_env_on("KL_LOWMEM_USAGE_CHANGED", t && strcmp(t, "x9386827918099701") == 0);
}
int kl_jni_low_memory(void) {
    const klj_native *n = NULL, *usage = NULL;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_nnatives; i++) {
        if (strcmp(g_natives[i].name, "nativeLowMemory") == 0) { n = &g_natives[i]; break; }
        if (!usage && strcmp(g_natives[i].name, "nativeMemoryUsageChanged") == 0 &&
            strcmp(g_natives[i].sig, "(I)V") == 0 &&
            (strcmp(g_natives[i].cls, "com/unity3d/player/UnityPlayerForActivityOrService") == 0 ||
             strcmp(g_natives[i].cls, "com/unity3d/player/UnityPlayer") == 0))
            usage = &g_natives[i];
    }
    pthread_mutex_unlock(&g_lock);
    if (!n) {
        void *player = usage ? klj_unity_player_made() : NULL;
        if (!player || !klj_lowmem_usage_changed()) return 0;
        ((void (*)(void *, void *, kl_jint))usage->fn)(kl_jni_env(), player, 4);
        return 1;
    }
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
    if (g_loose_on)
        fprintf(out, "  frameless strings: %llu retired by age (ring of %d, %u held)\n",
                (unsigned long long)g_stat_loose, KLJ_LOOSE_MAX, g_loose_n);
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
// and a jobject is already a raw pointer. Exception state is one pending
// Throwable per thread (klj_raise, Throw, ThrowNew, below): clear unless the
// host raised one where Java would have — a ZipFile that is not there, a class
// an APK does not ship — or the guest threw one itself. Anything that would
// require us to invent Java semantics stays an abort stub.

static void *klj_GetVersion(void *env) { (void)env; return (void *)(uintptr_t)KL_JNI_VERSION_1_6; }

// Classes the APK does NOT ship, for a guest that probes for them. FindClass
// here interns any name (see the Sentry note in kl_jni_services.c), which is
// right for the Android framework and wrong for an application's OPTIONAL
// dependency, where absence is the answer. Pistol Whip's Quest build carries
// the PICO SDK's C# side but not its Java side (classes.dex has no
// com/psmart/...): on a Quest `new AndroidJavaClass("com.psmart.aosoperation.
// SysActivity")` throws and the game takes the Oculus path. Answered with a
// class, it decides it is on a PICO, loads PxrPlatform (not in the APK),
// throws DllNotFoundException and never starts the Oculus XR display —
// frames run, every one black. Per target, like the ZipFile throw.
int klj_class_absent(const char *name) {
    // Not a guess for every guest: whether a class exists is a property of the
    // APK, and the rule that would say it for all of them is the APK's own
    // class list (classes*.dex), which nothing here reads yet.
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        if (!t) return 0;                                  // asked before the target is known: not settled yet
        on = strcmp(t, "x2104963472963790") == 0;          // Pistol Whip
    }
    return on && name && strncmp(name, "com/psmart/", 11) == 0;
}

static void *klj_FindClass(void *env, const char *name) {
    (void)env;
    if (klj_class_absent(name)) {
        // JNI's FindClass returns NULL with NoClassDefFoundError pending.
        klj_raise("java/lang/NoClassDefFoundError", name);
        return NULL;
    }
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

// A pointer that is not a LIVE jobject but is nonetheless one of our pool slots
// is a RETIRED one: klj_retire_object_locked zeroes the magic and leaves
// everything else standing, and the slot is only quarantined, never unmapped.
// So the class it last held is still readable, and that is the whole diagnosis.
static const char *klj_retired_slot(void *p, unsigned *idx) {
    if (!p) return NULL;
    char *base = (char *)g_objects, *q = (char *)p;
    if (q < base || q >= base + sizeof g_objects) return NULL;
    if ((size_t)(q - base) % sizeof g_objects[0]) return NULL;   // not a slot start
    klj_object *o = (klj_object *)p;
    if (idx) *idx = (unsigned)(o - g_objects);
    return o->cls;
}

 void *klj_GetObjectClass(void *env, void *obj) {
    (void)env;
    klj_object *o = klj_as_object(obj);
    if (!o) {
        KLJ_LOG("GetObjectClass on an untagged pointer %p — every jobject the guest "
                "holds should have come from us", obj);
        // ...unless it DID come from us and has since been retired, which is a
        // different bug with a different fix, and the pool can tell the two
        // apart: a retired slot is still one of ours, still carries the class
        // it last held, and only its magic is gone. Saying so turns a bare
        // pointer into the binding that needs klj_host_cache (or the guest
        // holding a local ref past its frame).
        unsigned slot; const char *was = klj_retired_slot(obj, &slot);
        if (was)
            KLJ_LOG("  it is our object pool slot %u, class %s, RETIRED — a local "
                    "frame popped it while something still held the pointer; a "
                    "host static caching it must go through klj_host_cache", slot, was);
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
        // KL_JNI_SOFT_NULLCLASS survives this specific abort WITHOUT the global
        // KL_PERMISSIVE (which would make every OVRPlugin getter return 0 and
        // black out the XR surface). AC Nexus's UbiServices entitlement check
        // in the menu scene hits GetObjectClass(NULL); answering Object lets the
        // NEXT call (a GetMethodID) name the method it wanted, turning "some
        // jobject is NULL" into a concrete UbiServices JNI call to implement.
        // Default ON for AC Nexus — the target this branch was written from
        // (its UbiServices SDK hits GetObjectClass(NULL) on every boot, and the
        // knob had to be remembered by hand on every run; one forgotten run
        // read as a fresh crash). KL_JNI_SOFT_NULLCLASS=0 still turns it off.
        int soft_default = 0;
        {
            const char *t = kl_driver_target_name();
            if (t && strcmp(t, "acnexusvr") == 0) soft_default = 1;
            // ...and the same game as a store install, where the target is
            // named after its Meta app id rather than "acnexusvr".
            if (t && strcmp(t, "x5812519008825194") == 0) soft_default = 1;
            // hl2's libsourcevr does GetObjectClass on an untagged pointer while
            // reading a saved setting (getSharedPreferences -> getInt) in the menu
            // — the same shape as AC Nexus's UbiServices check. Answering Object
            // lets it past (or the next call names what it wanted) instead of
            // aborting the whole run on one menu preference read.
            if (t && strcmp(t, "hl2") == 0) soft_default = 1;
        }
        if (kl_permissive() || kl_env_on("KL_JNI_SOFT_NULLCLASS", soft_default)) {
            KLJ_LOG("soft null-class: answering java/lang/Object so the NEXT call "
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

// Pending Java exceptions, per thread as in a real VM. Almost nothing raises
// one: a missing answer here has always been a NULL return, and that stays the
// rule. klj_raise() is for the few calls whose THROW is the answer the guest
// reads — Unity's OBB GUID check learns that patch.<code>.<package>.obb is
// absent only from `new ZipFile()` throwing FileNotFoundException; with no
// exception it takes the "patch exists" branch, reads its GUID as '' over the
// main OBB's good one, and stops on "Application OBB has mismatching GUID".
// With nothing raised these four are exactly the old constants.
static __thread klj_object *t_pending_exc;

void klj_raise(const char *cls, const char *msg) {
    // The message rides in the object, for Throwable.getMessage/toString: the
    // guest logs what it caught (Unity calls toString on it straight away).
    void *o = klj_own(klj_new_object_data(cls, strdup(msg ? msg : "")), free);
    pthread_mutex_lock(&g_lock);
    klj_object *e = klj_as_object(o);
    if (e) e->pinned++;                 // lives until ExceptionClear, not a frame pop
    klj_object *old = t_pending_exc;
    t_pending_exc = e;
    if (old && old->pinned) old->pinned--;
    pthread_mutex_unlock(&g_lock);
    KLJ_LOG("throw %s: %s", cls, msg ? msg : "");
}

// The guest's own throws. Unity catches what klj_raise() threw, logs it, and
// hands it on with Throw — for its caller to catch — so these set the same
// per-thread state. Both return 0 (JNI_OK).
static void klj_set_pending(klj_object *e) {
    pthread_mutex_lock(&g_lock);
    if (e) e->pinned++;
    klj_object *old = t_pending_exc;
    t_pending_exc = e;
    if (old && old->pinned) old->pinned--;
    pthread_mutex_unlock(&g_lock);
}
static kl_jint klj_Throw(void *env, void *obj) {
    (void)env;
    klj_object *e = klj_as_object(obj);
    if (!e) return -1;
    KLJ_LOG("Throw %s (from the guest)", e->cls);
    klj_set_pending(e);
    return 0;
}
static kl_jint klj_ThrowNew(void *env, void *clazz, const char *msg) {
    (void)env;
    const char *cls = klj_class_name(clazz);
    klj_object *e = klj_as_object(klj_own(klj_new_object_data(cls, strdup(msg ? msg : "")), free));
    if (!e) return -1;
    KLJ_LOG("ThrowNew %s: %s (from the guest)", cls, msg ? msg : "");
    klj_set_pending(e);
    return 0;
}

static void *klj_ExceptionOccurred(void *env) { (void)env; return t_pending_exc; }
static void  klj_ExceptionClear(void *env) {
    (void)env;
    klj_object *e = t_pending_exc;
    if (!e) return;
    t_pending_exc = NULL;
    pthread_mutex_lock(&g_lock);
    if (e->pinned) e->pinned--;
    pthread_mutex_unlock(&g_lock);
    KLJ_LOG("exception %s cleared by the guest", e->cls);
}
// Java's ExceptionDescribe prints and clears.
static void  klj_ExceptionDescribe(void *env) {
    if (t_pending_exc) KLJ_LOG("ExceptionDescribe: %s", t_pending_exc->cls);
    klj_ExceptionClear(env);
}
 kl_jint klj_ExceptionCheck(void *env)  { (void)env; return t_pending_exc != NULL; }

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

// The same shape from a UE4 guest building a String[]: AcquirePermissions
// makes each name with a scoped ToJavaString, stores it with
// SetObjectArrayElement and DeleteLocalRefs it at the end of the loop — on the
// game thread, with no frame open — so on Android the array keeps every name
// alive and here every name was retired before PermissionHelper read it
// ("expected a java/lang/String, got an untagged pointer", 32 bytes apart).
// KL_JNI_DEFER_STRING_DELETE: a deleted String is retired by age instead,
// through the frameless-string ring (klj_loose_note), and a frame's String by
// its pop. Default on for The Walking Dead: Saints & Sinners - Chapter 2
// (x5190288877703616) alone, whose loading waits on that permission answer.
static int klj_defer_string_delete(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_JNI_DEFER_STRING_DELETE",
                       t && strcmp(t, "x5190288877703616") == 0);
    }
    return on;
}
static void klj_loose_note(klj_object *o);

static void  klj_DeleteLocalRef(void *env, void *obj) {
    (void)env;
    if (!obj) return;                       // DeleteLocalRef(NULL) is legal
    pthread_mutex_lock(&g_lock);
    klj_object *o = klj_as_object(obj);
    if (o) g_stat_delete++;
    int defer = o && o->cls == KLJ_CLASS_STRING && !o->pinned && klj_defer_string_delete();
    if (!defer && (!t_frame || !o || klj_frame_holds(o))) klj_retire_object_locked(obj);
    pthread_mutex_unlock(&g_lock);
    if (defer && !t_frame) klj_loose_note(o);
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
    {"com/unity3d/player/UnityPlayer", "android/widget/FrameLayout"},
    // Unity 2023+ declares the lifecycle natives on this subclass (see
    // unity_lifecycle_native in kl_driver.c). A method looked up on it and
    // declared against UnityPlayer is UnityPlayer's.
    {"com/unity3d/player/UnityPlayerForActivityOrService", "com/unity3d/player/UnityPlayer"},
    {"android/widget/FrameLayout", "android/view/ViewGroup"},
    {"android/view/ViewGroup", "android/view/View"},
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
    // UE4's GameActivity extends NativeActivity, so everything an Activity or a
    // Context answers is answerable through it — and the guest calls those
    // through the GameActivity class it holds. Without this edge only the rows
    // keyed on GameActivity itself resolved: Red Matter aborted on
    // GameActivity.getPackageName() deep into play (2026-10-12), with
    // Context.getPackageName bound all along.
    {"com/epicgames/ue4/GameActivity",         "android/app/NativeActivity"},
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
    // What klj_raise() throws, so a guest that catches the base class (or asks
    // IsInstanceOf IOException) recognises it.
    {"java/io/FileNotFoundException",          "java/io/IOException"},
    {"java/lang/NoClassDefFoundError",         "java/lang/LinkageError"},
    {"java/lang/ClassNotFoundException",       "java/lang/ReflectiveOperationException"},
    {"java/lang/ReflectiveOperationException", "java/lang/Exception"},
    {"java/lang/LinkageError",                 "java/lang/Error"},
    {"java/lang/Error",                        "java/lang/Throwable"},
    {"java/util/zip/ZipException",             "java/io/IOException"},
    {"java/io/IOException",                    "java/lang/Exception"},
    // ...and what a guest throws of its own accord (ThrowNew), or catches by
    // the base class: without these a RuntimeException was no Throwable here,
    // its message was lost, and a catch of java.lang.Exception passed it by.
    {"java/lang/IllegalStateException",        "java/lang/RuntimeException"},
    {"java/lang/IllegalArgumentException",     "java/lang/RuntimeException"},
    {"java/lang/NullPointerException",         "java/lang/RuntimeException"},
    {"java/lang/UnsupportedOperationException","java/lang/RuntimeException"},
    {"java/lang/IndexOutOfBoundsException",    "java/lang/RuntimeException"},
    {"java/lang/SecurityException",            "java/lang/RuntimeException"},
    {"java/lang/ClassCastException",           "java/lang/RuntimeException"},
    {"java/lang/RuntimeException",             "java/lang/Exception"},
    {"java/lang/NoSuchMethodError",            "java/lang/LinkageError"},
    {"java/lang/NoSuchFieldError",             "java/lang/LinkageError"},
    {"java/lang/UnsatisfiedLinkError",         "java/lang/LinkageError"},
    {"java/lang/Exception",                    "java/lang/Throwable"},
    {NULL, NULL},
};

// Does `cls` reach `want` by walking superclasses? The one answer behind both
// IsInstanceOf (which asks it about an object's class) and the assignability
// tier of klj_find_binding (which asks it about two parameter descriptors).
// Factored out of IsInstanceOf rather than copied: two walks over g_supers that
// could disagree is exactly the drift g_supers exists to prevent.
static int klj_class_is_a(const char *cls, const char *want) {
    if (!cls || !want) return 0;
    if (strcmp(want, "java/lang/Object") == 0) return 1;
    for (const char *cur = cls; cur; ) {
        if (strcmp(cur, want) == 0) return 1;
        const char *next = NULL;
        for (int i = 0; g_supers[i].cls; i++)
            if (strcmp(g_supers[i].cls, cur) == 0) { next = g_supers[i].super; break; }
        cur = next;
    }
    return 0;
}

static kl_jint klj_IsInstanceOf(void *env, void *obj, void *clazz) {
    (void)env;
    if (!obj) return 1;                       // null is an instance of everything
    klj_object *o = klj_as_object(obj);
    if (!o) return 0;
    return klj_class_is_a(o->cls, klj_class_name(clazz));
}

// Can an object of clazz1 be cast to clazz2? Unity's reflection layer asks
// this while matching argument types (HARD BULLET reaches it right after
// loading liblck_core, on the main-menu path).
static kl_jint klj_IsAssignableFrom(void *env, void *clazz1, void *clazz2) {
    (void)env;
    kl_jint r = klj_class_is_a(klj_class_name(clazz1), klj_class_name(clazz2));
    KLJ_LOG("IsAssignableFrom(%s, %s) -> %d",
            klj_class_name(clazz1), klj_class_name(clazz2), r);
    return r;
}

// What a class answers a method with ITSELF — its own binding or an ancestor's,
// never java/lang/Object's catch-all. For Object.toString, which is resolved on
// Object and called on whatever the object is.
static const klj_binding *klj_find_binding(const char *cls, const char *name, const char *sig);
klj_impl klj_own_impl(const char *cls, const char *name, const char *sig) {
    for (const char *cur = cls; cur && strcmp(cur, "java/lang/Object") != 0; ) {
        const klj_binding *b = klj_find_binding(cur, name, sig);
        if (b) return b->fn;
        const char *next = NULL;
        for (int i = 0; g_supers[i].cls; i++)
            if (strcmp(g_supers[i].cls, cur) == 0) { next = g_supers[i].super; break; }
        cur = next;
    }
    return NULL;
}

// The message of an exception klj_raise() made, or NULL for any other object.
const char *klj_throwable_message(void *obj) {
    klj_object *o = klj_as_object(obj);
    if (!o || !o->data) return NULL;
    return klj_IsInstanceOf(NULL, obj, klj_class_object("java/lang/Throwable"))
        ? (const char *)o->data : NULL;
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
// Strings a guest makes on a thread with no local frame, and never deletes.
//
// A native thread that attached itself has no frame to pop: on Android its local
// references live until it deletes them or detaches, and ART's table is large
// enough that a guest can leak one per call for hours. The pool here is not.
// Doom3Quest's haptics do exactly that — NewStringUTF for the event name on
// every haptic_updateevent/stopevent, some 300 a second in a level, none ever
// deleted (host run, 2026-10-08: 45423 live java/lang/String after 150 s) —
// which walks KLJ_MAX_OBJECTS to "object pool exhausted" and an abort after
// about seven minutes of play (device crash report 2026-10-06 19:15).
//
// So such strings are kept in a ring and the oldest is RETIRED when the ring is
// full: KLJ_LOOSE_MAX strings later, and then through the usual quarantine
// before its slot or its bytes are reused. A guest would have to come back to a
// string it made tens of thousands of strings ago to notice. Only for a door
// that asks (kl_jni_reclaim_frameless_strings) — where the host makes its calls
// inside frames this ring would never fill, but a host-side cache of an
// unpinned frameless string is not something to find out about this way.
void kl_jni_reclaim_frameless_strings(int on) { g_loose_on = on; }

static void klj_loose_note(klj_object *o) {
    pthread_mutex_lock(&g_lock);
    if (g_loose_n == KLJ_LOOSE_MAX) {
        klj_object *old = &g_objects[g_loose_slot[g_loose_head]];
        // Still the string that was noted? The slot may have been deleted and
        // recycled since; its payload pointer says which occupant this is.
        if (old->magic == KLJ_OBJ_MAGIC && !old->pinned &&
            old->cls == KLJ_CLASS_STRING && old->data == g_loose_data[g_loose_head]) {
            klj_retire_object_locked(old);
            g_stat_loose++;
        }
        g_loose_head = (g_loose_head + 1) % KLJ_LOOSE_MAX;
        g_loose_n--;
    }
    unsigned at = (g_loose_head + g_loose_n) % KLJ_LOOSE_MAX;
    g_loose_slot[at] = (unsigned)(o - g_objects);
    g_loose_data[at] = o->data;
    g_loose_n++;
    pthread_mutex_unlock(&g_lock);
}

static void *klj_NewStringUTF(void *env, const char *s) {
    (void)env;
    if (!s) return NULL;
    void *o = kl_jni_new_string(s);
    if (g_loose_on && !t_frame && o) klj_loose_note(o);
    return o;
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

// Defined below with the rest of the binding matcher; klj_want needs it to
// resolve a lookup the guest made without a signature (see there).
static const klj_binding *klj_resolve_by_name(const char *cls, const char *name,
                                              int *nmatch);

// The measurement instrument. Every id the guest wants is recorded; in strict
// mode the first unknown one stops the run, which is the bring-up loop.
// Permissive mode hands back a synthetic id so a single run
// collects the whole batch of lookups an init path performs.
 void *klj_want(void *clazz, const char *name, const char *sig, char kind) {
    const char *cls = klj_class_name(clazz);
    if (!sig) sig = "";

    // ---- the guest asked with NO signature at all ----
    //
    // Unity's `AndroidJNIHelper.GetMethodID(jclass, name, signature = "",
    // isStatic = false)` takes the signature as an OPTIONAL argument. Omit it
    // and the managed side does not build one: it passes "" through to
    // `com.unity3d.player.ReflectionHelper.getMethodID`, which on device
    // resolves the method REFLECTIVELY — `getMethods()` filtered by name — and
    // returns FromReflectedMethod. The signature is never spelled anywhere.
    //
    // bHaptics' Unity wrapper caches its ids both ways in one breath: with an
    // explicit signature for most of BhapticsManagerWrapper and without one for
    // five (`stopInt`, `stopAll`, `pingAll`, `isBhapticsUser`, `refreshPairing`),
    // so the same class arrives spelled both ways in a single boot.
    //
    // An empty signature has no return kind (klj_return_kind gives '?') and no
    // argument list, so it matches no binding and decodes no arguments. The id
    // is issued happily — a lookup is only a measurement — and the run then dies
    // at the CALL, on a method the table already implements, which is a
    // confusing place to land: with `refreshPairing ()V` in the table and the
    // dex agreeing, Paint the Town Red VR's call still aborted.
    //
    // Resolve it the way the device does: by (class, name) alone, and only when
    // the answer is UNIQUE. Substituting the resolved signature here — before
    // the id is interned — is what makes the rest of this file need to know
    // nothing: the return-kind check, klj_decode_args and klj_resolve_binding
    // all read w->sig and now see the real thing, and a later lookup that DOES
    // spell the signature interns to the same entry rather than a second one.
    //
    // Ambiguity is refused, not guessed: `isPlayingByEventId` is declared twice
    // (int and String) and picking either would marshal one as the other. A
    // refusal leaves the empty signature in place, which is exactly today's
    // behaviour — the call names the method and stops.
    if ((kind == 'm' || kind == 'M') && !*sig) {
        int n = 0;
        const klj_binding *b = klj_resolve_by_name(cls, name, &n);
        if (b)
            KLJ_LOG("%s asked for %s.%s with NO signature (Unity's optional-arg "
                    "spelling); resolved by name to %s",
                    kind == 'M' ? "GetStaticMethodID" : "GetMethodID",
                    cls, name, b->sig);
        else if (n > 1)
            KLJ_LOG("%s.%s asked with NO signature is AMBIGUOUS: %d bindings share "
                    "that name — resolving by name would guess which one to "
                    "marshal, so it is refused", cls, name, n);
        else
            KLJ_LOG("%s.%s asked with NO signature and no binding carries that "
                    "name — nothing to resolve it to", cls, name);
        if (b) sig = b->sig;
    }

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
        if (*p == '[') {
            // Every array is a jobject, including float[] and double[].
            // Its element type must never select the FP argument registers.
            while (*p == '[') p++;
            if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
            else if (*p) p++;
            else return -1;
            argv[argc].l = (void *)(uintptr_t)kl_va_gp(va);
            argc++;
            continue;
        }
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
        if (*p == '[') {
            // Every array is a jobject, including float[] and double[].
            // Its element type must never select the FP argument registers.
            while (*p == '[') p++;
            if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
            else if (*p) p++;
            else return -1;
            argv[argc].l = args[argc].l;
            argc++;
            continue;
        }
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

// ---- the third tier: a parameter the guest spelled as a SUBCLASS ----
//
// The two tiers above compare argument lists character for character, and that
// is not what Java does. `Class.getMethod(name, argTypes)` resolves by
// ASSIGNABILITY: a method declared `init(Context)` is the method you get when
// you ask with an Activity, because an Activity IS a Context.
//
// Unity's C# side asks exactly that way. `AndroidJavaClass.CallStatic` builds
// the signature from the RUNTIME type of the object it was handed, so HARD
// BULLET's FMOD wrapper asks for
// `org/fmod/FMOD.init(Lcom.unity3d.player.UnityPlayerGameActivity;)V` where
// FMOD.java declares `init(Landroid/content/Context;)V`. Nothing is wrong with
// either spelling; on device the reflective lookup bridges them. Without this
// tier every such call is a separate abort, and the "fix" is transcribing the
// same binding once per subclass the guest happens to hold — which is the
// duplication g_supers already exists to avoid.
//
// The relaxation is kept narrow so it cannot become a silent wrong answer:
//   - arity must match, and the RETURN must match exactly (the tier above
//     already owns the generic-return case);
//   - primitives and arrays must match exactly — only `L…;` widens;
//   - widening is the g_supers walk and nothing else, the same walk
//     IsAssignableFrom answers with;
//   - at least one parameter must genuinely differ, so this tier can never
//     shadow an exact match;
//   - and the candidate must be UNIQUE. Two bindings both assignable is a real
//     overload set (`f(Context)` and `f(Activity)`), where picking either is a
//     guess; that is refused and named rather than resolved by table order.
static const char *klj_type_end(const char *p) {
    while (*p == '[') p++;
    if (*p == 'L') { const char *e = strchr(p, ';'); return e ? e + 1 : NULL; }
    if (!*p || *p == ')') return NULL;
    return p + 1;
}

// `from` fits in a `to` slot. Both are one descriptor, delimited by [ps,pe).
//
// One extra rule beyond the g_supers walk: a guest argument spelled EXACTLY
// `Ljava/lang/Object;` fits any reference slot. That is not Java assignability
// — Object is not a Handler — it is the ERASED spelling, and it is the same
// fact the generic-return tier above already accepts on the other side of the
// parens. Unity's `AndroidJNIHelper.GetSignature(object)` has no type to read
// when the argument is `null` (or is held in an `object`/`AndroidJavaProxy`
// variable), so it emits `Ljava/lang/Object;` and the managed caller asks for
// a method the native caller asks for correctly. Paint the Town Red's Rewired
// input source does exactly this:
//
//     InputManager.registerInputDeviceListener(
//         Landroid.hardware.input.InputManager$InputDeviceListener;
//         Ljava/lang/Object;)V          <- the null Handler, erased
//
// where libunity, in the same boot, resolved the identical method as
// `(...;Landroid/os/Handler;)V`. Without this rule every erased-argument call
// is a separate abort whose only fix is a duplicate table entry per call site.
// It stays safe for the same reason the tiers above do: an exact match still
// wins outright, at least one parameter must have widened, and a signature
// that two bindings both accept is refused as AMBIGUOUS rather than guessed.
static int klj_type_assignable(const char *from, const char *fe,
                               const char *to, const char *te) {
    size_t lf = (size_t)(fe - from), lt = (size_t)(te - to);
    if (lf == lt && klj_sig_eq_n(from, to, lf)) return 1;
    // The erased spelling: only ever accepted INTO a reference slot, never into
    // a primitive one, so an Object can never be marshalled as an int.
    if (lf == 18 && klj_sig_eq_n(from, "Ljava/lang/Object;", 18) &&
        (*to == 'L' || *to == '[')) return 1;
    if (*from != 'L' || *to != 'L') return 0;     // primitive/array: exact only
    char f[192], t[192];
    if (lf < 3 || lf >= sizeof f || lt < 3 || lt >= sizeof t) return 0;
    size_t i;
    for (i = 0; i < lf - 2; i++) f[i] = from[1 + i] == '.' ? '/' : from[1 + i];
    f[i] = 0;
    for (i = 0; i < lt - 2; i++) t[i] = to[1 + i] == '.' ? '/' : to[1 + i];
    t[i] = 0;
    return klj_class_is_a(f, t);
}

// The table declares `tbl`; the guest asked for `guest`. Returns 1 only if the
// guest's arguments widen into the table's AND something actually widened.
static int klj_args_assignable(const char *tbl, const char *guest) {
    const char *a = strchr(tbl, '('), *b = strchr(guest, '(');
    if (!a || !b) return 0;
    a++; b++;
    int widened = 0;
    while (*a && *a != ')' && *b && *b != ')') {
        const char *ae = klj_type_end(a), *be = klj_type_end(b);
        if (!ae || !be) return 0;
        if (!klj_type_assignable(b, be, a, ae)) return 0;
        size_t la = (size_t)(ae - a), lb = (size_t)(be - b);
        if (la != lb || !klj_sig_eq_n(a, b, la)) widened = 1;
        a = ae; b = be;
    }
    if (*a != ')' || *b != ')') return 0;         // different arity
    if (!widened) return 0;                       // never shadow an exact match
    // The returns must agree — exactly, or the way the tier above already
    // accepts them: both references. Java has no return-type overloading, so a
    // reference return spelled two ways is still one method, and a guest that
    // erased an ARGUMENT is exactly the guest that erases the RETURN in the
    // same breath (`AndroidJavaObject.Call<AndroidJavaObject>(name, null)`
    // erases both ends of the same call). Requiring an exact return here made
    // that doubly-erased call resolve through neither tier: the loose tier
    // wants identical arguments, this one wanted an identical return, and the
    // one call that needs both relaxations at once fell between them.
    return klj_sig_eq(a, b) || klj_same_args(a, b);
}

static const klj_binding *klj_find_binding(const char *cls, const char *name,
                                           const char *sig) {
    // One title's own answers first. Each of these is NULL unless the target
    // is the one it was written for, so no other game's lookup changes.
    const klj_binding *own = klj_firebase_binding(cls, name, sig);
    if (!own) own = klj_vivox_binding(cls, name, sig);
    if (!own) own = klj_vertigo_binding(cls, name, sig);
    if (!own) own = klj_ghosttown_binding(cls, name, sig);
    if (own) return own;
    // Exact wins outright across EVERY family table, so the answer does not
    // depend on which file a class landed in; a loose match is only ever the
    // fallback.
    const klj_binding *loose = NULL, *wide = NULL;
    int nwide = 0;
    for (const klj_binding *const *t = klj_binding_tables; *t; t++)
        for (const klj_binding *b = *t; b->cls; b++) {
            if (strcmp(b->cls, cls) != 0 || strcmp(b->name, name) != 0) continue;
            if (klj_sig_eq(b->sig, sig)) return b;
            if (!loose && klj_same_args(b->sig, sig)) loose = b;
            if (klj_args_assignable(b->sig, sig)) { if (!wide) wide = b; nwide++; }
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
    if (loose) return loose;

    // Named on the same terms and for the same reason: a widened match is the
    // right answer for the subclass-argument case and a silent wrong one for
    // anything else, so it goes on the record.
    if (wide) {
        if (nwide > 1) {
            KLJ_LOG("binding %s.%s%s is AMBIGUOUS: %d table entries accept it by "
                    "assignability — declare the exact signature", cls, name, sig, nwide);
            return NULL;
        }
        static const char *seen[64];
        static unsigned nseen;
        unsigned i = 0;
        for (; i < nseen; i++) if (seen[i] == wide->sig) break;
        if (i == nseen && nseen < 64) {
            seen[nseen++] = wide->sig;
            KLJ_LOG("binding %s.%s matched by ASSIGNABILITY: guest asked %s, "
                    "table has %s", cls, name, sig, wide->sig);
        }
    }
    return wide;
}

static const klj_binding *klj_resolve_binding(const char *cls, const char *name,
                                              const char *sig) {
    // UE5 renamed the activity package com.epicgames.ue4 -> com.epicgames.unreal
    // (Wanderer, libUnreal). The whole AndroidThunkJava_* binding surface is
    // keyed on the ue4 name, so alias the unreal class onto it rather than
    // duplicating ~40 rows — the natives are identical, only the package moved.
    // UE5 renamed the whole com.epicgames.ue4.* package to com.epicgames.unreal.*
    // (Wanderer, OLAR). The binding surface is keyed on the ue4 spelling, so alias
    // ANY com/epicgames/unreal/X onto com/epicgames/ue4/X — GameActivity,
    // MessageBox01, the receivers, MediaPlayer14 — rather than duplicating rows.
    if (strncmp(cls, "com/epicgames/unreal/", 21) == 0) {
        char alias[256];
        snprintf(alias, sizeof alias, "com/epicgames/ue4/%s", cls + 21);
        const klj_binding *b = klj_find_binding(alias, name, sig);
        if (b) return b;
    }
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

// ---- the fourth tier: no signature at all ----
//
// Only ever reached from klj_want, for the empty-signature lookup documented
// there. It is deliberately NOT part of klj_find_binding: a call always has a
// signature by the time it gets there, and a name-only fallback at call time
// would paper over a genuine mismatch. Here it is the whole of the information
// the guest supplied.
//
// "Unique" is counted over distinct SIGNATURES, not table rows, so a method
// deliberately declared in two family tables with the same spelling still
// resolves; two different spellings is an overload set and is refused.
static const klj_binding *klj_find_by_name(const char *cls, const char *name,
                                           int *nmatch) {
    const klj_binding *first = NULL;
    const char *seen[16];
    int n = 0;
    for (const klj_binding *const *t = klj_binding_tables; *t; t++)
        for (const klj_binding *b = *t; b->cls; b++) {
            if (strcmp(b->cls, cls) != 0 || strcmp(b->name, name) != 0) continue;
            int dup = 0;
            for (int i = 0; i < n && i < 16; i++)
                if (klj_sig_eq(seen[i], b->sig)) { dup = 1; break; }
            if (dup) continue;
            if (n < 16) seen[n] = b->sig;
            if (!first) first = b;
            n++;
        }
    *nmatch = n;
    return n == 1 ? first : NULL;
}

// The same superclass walk klj_resolve_binding does, and it stops at the first
// class in the chain that declares the name at all — a subclass that overloads
// a name shadows the superclass's, exactly as Java resolution does, rather than
// pooling both into a false ambiguity.
static const klj_binding *klj_resolve_by_name(const char *cls, const char *name,
                                              int *nmatch) {
    for (const char *cur = cls; cur; ) {
        int n = 0;
        const klj_binding *b = klj_find_by_name(cur, name, &n);
        if (n) { *nmatch = n; return b; }
        const char *next = NULL;
        for (int i = 0; g_supers[i].cls; i++)
            if (strcmp(g_supers[i].cls, cur) == 0) { next = g_supers[i].super; break; }
        cur = next;
    }
    return klj_find_by_name("java/lang/Object", name, nmatch);
}

// The Sentry crash reporter's Java SDK. Arizona Sunshine 2
// (x5245041552210029) ships sentry-unity, whose Android integration asks the
// Java side for its hub (Sentry.getCurrentHub) right after the splash, then
// sets tags, builds Breadcrumbs (writing their fields) and reads SentryLevel
// constants — each of which aborted the run. There is no Sentry Java SDK
// running on this host, so every io.sentry call and field read answers
// zero/NULL/void and a field write is not kept, as an uninitialised SDK does:
// a NULL hub is what the C# wrapper's `hub?.Call(...)` chains already treat
// as "Java SDK not enabled". Nothing is sent anywhere.
// KL_JNI_DROP_SENTRY=0 restores the stop here, =1 tries the drop elsewhere.
static int klj_drop_sentry(const klj_wanted *w) {
    if (strncmp(w->cls, "io/sentry/", 10) != 0) return 0;
    static int drop = -1;
    if (drop < 0) {
        const char *t = kl_driver_target_name();
        drop = kl_env_on("KL_JNI_DROP_SENTRY", t && strcmp(t, "x5245041552210029") == 0);
    }
    if (!drop) return 0;
    static int said;   // the first few name the entries; the rest are noise
    if (said < 16) {
        said++;
        KLJ_LOG("%s.%s%s — Sentry Java SDK, dropped (not running on this host)",
                w->cls, w->name, w->sig);
    }
    return 1;
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

    // Third-party analytics SDKs have no host to report to, and nothing a guest
    // does with their answers is load-bearing: every call is dropped and answers
    // zero/NULL/void, as a disabled SDK would. A whole class family rather than
    // one row per method, because the guest's C# wrapper reaches for whichever
    // entry its own error hook or event fires next. Max Mustard
    // (x5269080709783454) aborted here on GameAnalytics.addErrorEvent right
    // after its splash; HARD BULLET (x8116626111777808) on
    // AmplitudePlugin.setLibraryName when Start is pressed in its main menu;
    // LES MILLS BODYCOMBAT (x4015163475201433) on Cognitive3D's
    // com/c3d/androidjavaplugin/Plugin.initSessionData during its first scene.
    // KL_JNI_DROP_ANALYTICS=0 restores the abort.
    static const char *const k_dropped[] = { "com/gameanalytics/",
                                             "com/amplitude/",
                                             "com/c3d/" };
    for (unsigned i = 0; i < sizeof k_dropped / sizeof *k_dropped; i++)
        if (strncmp(w->cls, k_dropped[i], strlen(k_dropped[i])) == 0 &&
            kl_env_on("KL_JNI_DROP_ANALYTICS", 1)) {
            static int said;   // the first few name the entries; the rest are noise
            if (said < 16) {
                said++;
                KLJ_LOG("%s.%s%s — analytics SDK, dropped (no host to report to)",
                        w->cls, w->name, w->sig);
            }
            return zero;
        }

    // The bHaptics entries the family table does not have. Its 40 rows are one
    // version of the plugin's wrapper, read from Paint the Town Red's APK
    // (runtime/jni/kl_jni_haptics.c). Real VR Fishing (x2582932495064035) has
    // the plugin too, in a version nobody here has the files of to compare,
    // and its sender ran it on a headset with every com.bhaptics call
    // answering zero/NULL/void (fix 37, 2026-10-14). So for this title what the
    // table does not answer is dropped that way, as a vest that is not there,
    // instead of ending the run by name — and the names are logged, which is
    // the list of rows to add. Every other guest keeps the stop.
    // KL_JNI_DROP_BHAPTICS=0 restores it here, =1 tries the drop elsewhere.
    if (strncmp(w->cls, "com/bhaptics/", 13) == 0) {
        static int drop = -1;
        if (drop < 0) {
            const char *t = kl_driver_target_name();
            drop = kl_env_on("KL_JNI_DROP_BHAPTICS", t && strcmp(t, "x2582932495064035") == 0);
        }
        if (drop) {
            static int said;
            if (said < 16) {
                said++;
                KLJ_LOG("%s.%s%s — not in the bHaptics table, dropped (no vest on this host)",
                        w->cls, w->name, w->sig);
            }
            return zero;
        }
    }

    if (klj_drop_sentry(w)) return zero;

    KLJ_LOG("no host implementation for %s.%s%s", w->cls, w->name, w->sig);
    if (!g_permissive && klj_soft_unknown()) {
        KLJ_LOG("  ...answered with zero (KL_JNI_SOFT_UNKNOWN, the profile for untested games)");
        return zero;
    }
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

// Arrays live in host-owned contiguous storage; there is no moving JVM GC.
// Critical access borrows that exact storage, as the typed Elements calls do.
static void *klj_GetPrimitiveArrayCritical(void *env, void *array, uint8_t *is_copy) {
    (void)env;
    klj_object *o = klj_as_object(array);
    klj_array *a = o && o->cls[0] == '[' ? klj_arr(array) : NULL;
    if (!a || a->kind == 'L') {
        klj_raise("java/lang/IllegalArgumentException", "critical access needs a primitive array");
        return NULL;
    }
    if (is_copy) *is_copy = 0;
    return a->data;
}
static void klj_ReleasePrimitiveArrayCritical(void *env, void *array, void *data, kl_jint mode) {
    (void)env; (void)array; (void)data; (void)mode;
    // No copied buffer to commit or discard. JNI_ABORT does not undo direct writes.
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
    if (v) return v;
    // Per-target hardware identity. The default (g_fields) is a Quest 2; a title
    // that GATES on newer hardware refuses to run on it. batman (Batman: Arkham
    // Shadow) is Quest 3 / 3S exclusive: on the Quest 2 we present it pops "Your
    // device does not match the hardware requirements", abandons Vulkan, and falls
    // back to a GLES2 context whose shaders it does not ship (E/Unity "shader
    // compiler platform 5 is not available in shader blob") — so nothing renders
    // and the frame loop waits forever. Present a Quest 3 (codename eureka) for it.
    // Kept on the Android-10/API-29 base the rest of the surface is built for, so
    // only the hardware identity changes; if a run shows batman also gates on the
    // OS version, bump SDK_INT/RELEASE for it too.
    // Steam Link (steamlink-vr) as well, for a different reason: it tells the
    // Steam host which headset it is by Build.PRODUCT ("[DeviceHMD] Found device
    // static props for: hollywood"), and the host sizes the stream for that
    // headset's panel. As a Quest 2 the picture arrived as a 1152x4608 HEVC
    // stream, 2304x2304 an eye once unwarped, and was upscaled from there to a
    // display that is far finer — washed out on the Vision Pro (owner,
    // 2026-10-16). eureka is the finest panel in its hmd_config.json table.
    // Roboquest VR's Meta Store build (x9269393463181825, UE 5.6) at its
    // owner's request: as a Quest 2 it chose that headset's settings (its saved
    // settings name the Quest 2's 1920x1832 panel) and the scene looked short
    // of shadows (2026-10-23). The same Quest 3 everywhere it can ask: here,
    // the OpenXR system name (kl_openxr.c) and ovrp_GetSystemHeadsetType
    // (kl_ovrp.c).
    const char *t = kl_driver_target_name();
    if (t && (strcmp(t, "batman") == 0 || strcmp(t, "steamlink-vr") == 0 ||
              strcmp(t, "x9269393463181825") == 0)) {
        if (!strcmp(f->name, "MODEL"))    return "Quest 3";
        if (!strcmp(f->name, "DEVICE"))   return "eureka";
        if (!strcmp(f->name, "PRODUCT"))  return "eureka";
        if (!strcmp(f->name, "FINGERPRINT"))
            return "oculus/eureka/eureka:10/SQ3A.220605.009.A1/1:user/release-keys";
    }
    // Red Matter 2 decides what it runs on from the model first
    // (CCrossPlatform::InitPlatformData, read out of its libUE4): "Pacific",
    // "Quest" or "Pico Goblin", compared whole. Anything else is taken for a
    // PC, where the tracking system's name decides — and "OculusHMD" there
    // means its 3DoF platform: Is6DOFTracking() false. As "Quest 2" the game
    // therefore ran its 3DoF movement on the headset: a second turn in the
    // same direction turned back, snap or smooth, and the camera kept an
    // authored height (owner, 2026-10-13; the game's own state read from its
    // memory on a Mac: platform 3, 6dof 0 — with "Quest", 6dof 1). The Quest
    // and the Quest 2 it was written for both pass that comparison, and which
    // of the two it is it asks OVRPlugin afterwards (ovrp_GetSystemHeadsetType2,
    // still a Quest 2). For this title only: every other guest keeps the model
    // it was brought up with.
    if (t && strcmp(t, "redmatter2") == 0 && !strcmp(f->name, "MODEL")) return "Quest";
    // Vader Immortal I (x2108775495884888, UE 4.19) the same way: its
    // FAndroidMisc::SetVersionInfo sets IsRunningOnSantaCruz only for maker
    // "Oculus" with model "Quest", "Monterey" or "Standalone HMD", compared
    // whole. Off, the game still decided a shader precompile was due (its save
    // compares FOpenGLProgramBinaryCache's version hash) but read its progress
    // from UVRUtils::GetQuestShaderPrecompilePercent, which answers 0 off a
    // Quest — the loading panel stayed for good. The flag also picks the Quest
    // device profile and the Quest branch of its materials.
    // Episodes II and III are the same engine and the same check.
    if (t && (strcmp(t, "x2108775495884888") == 0 || strcmp(t, "x2223615721048141") == 0 ||
              strcmp(t, "x2426206484098337") == 0) && !strcmp(f->name, "MODEL")) return "Quest";
    return f->sval;
}

// Interned jstrings for constant object fields, parallel to g_fields so the
// table itself stays const. A static final String read twice is the same object
// in Java, and some callers do compare identity.
// Sized to sit AHEAD of the table, not behind it: an entry past the end falls
// to the uncached path, which mints an unpinned jstring per read — the exact
// use-after-retire the pinning above exists to prevent. The table has already
// grown past 128 once; 256 is a 2 KB array, so there is no reason to keep it
// tight enough to be overtaken again.
#define KLJ_MAX_FIELDS 256
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

klj_val klj_field_written(void *obj, void *fid) {
    const klj_val *value=klj_find_write(obj,fid);
    return value ? *value : (klj_val){0};
}

 void klj_field_store(void *obj, void *fid, klj_val v) {
    klj_wanted *w = fid;
    if (!w || w < g_wanted || w >= g_wanted + KLJ_MAX_WANTED) {
        KLJ_LOG("Set*Field with a jfieldID we never issued (%p)", fid);
        kl_jni_report(stderr);
    kl_egl_report(stderr);
        kl_fatal_prepare(); abort();
    }
    // Sentry's Breadcrumbs are filled field by field, one for every event the
    // game logs, and nothing ever reads them back: kept, they filled this
    // table in Arizona Sunshine 2's first level and ended the run.
    if (klj_drop_sentry(w)) return;
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
    // UE5's unreal-package GameActivity aliases onto ue4 for FIELDS too, the
    // same way klj_resolve_binding does for methods — g_fields keys the whole
    // GameActivity static surface (ANDROID_BUILD_VERSION, ...) on the ue4 name.
    const char *wcls = w->cls;
    if (strcmp(wcls, "com/epicgames/unreal/GameActivity") == 0)
        wcls = "com/epicgames/ue4/GameActivity";
    for (const klj_field *f = g_fields; f->cls; f++) {
        if (strcmp(f->cls, wcls) || strcmp(f->name, w->name) || strcmp(f->sig, w->sig))
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
    // Epic Online Services' enum constants (EOSOverlay$BrowserStatus.BEGIN_LOAD,
    // EOSLog$Level.*, ...). libEOSSDK reads every constant of its Java enums
    // at init and keeps them as global refs to hand back into its own Java
    // half — which is not here, so nothing ever looks inside them. What does
    // matter is that each constant is a distinct, stable, non-null object of
    // its own class, so one per (class, name) jfieldID, pinned for the
    // process. Narrowed to the EOS package and to the enum shape (a static
    // field whose type is its own class) so nothing else is answered blindly.
    if (w->kind == 'F' && want == 'L'
        && strncmp(w->cls, "com/epicgames/mobile/eossdk/", 28) == 0
        && w->sig[0] == 'L' && strncmp(w->sig + 1, w->cls, strlen(w->cls)) == 0
        && strcmp(w->sig + 1 + strlen(w->cls), ";") == 0) {
        static void *eos_consts[KLJ_MAX_WANTED];
        size_t idx = (size_t)(w - g_wanted);
        if (!eos_consts[idx]) {
            eos_consts[idx] = kl_jni_new_object(w->cls);
            klj_as_object(eos_consts[idx])->pinned = 1;
            KLJ_LOG("EOS enum constant %s.%s -> fresh pinned object", w->cls, w->name);
        }
        return (klj_val){.l = eos_consts[idx]};
    }
    {   // The Horizon Platform reply object: kl_jni_net.c owns its shape.
        klj_val hz;
        if (klj_horizon_message_field(obj, w->cls, w->name, w->sig, &hz)) return hz;
        if (klj_ue4_media_track_field(obj, w->cls, w->name, w->sig, &hz)) return hz;
    }
    if (klj_drop_sentry(w)) return (klj_val){0};
    KLJ_LOG("no host value for field %s.%s %s", w->cls, w->name, w->sig);
    if (!g_permissive && klj_soft_unknown()) {
        KLJ_LOG("  ...answered with zero (KL_JNI_SOFT_UNKNOWN, the profile for untested games)");
        return (klj_val){0};
    }
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
    // Space Atlas VR: the avatar SDK reads this platform constant before
    // checking permission. This supplies the name, not a permission grant.
    KLJ_FSTR("android/Manifest$permission", "READ_EXTERNAL_STORAGE",
             "android.permission.READ_EXTERNAL_STORAGE"),
    KLJ_FSTR("android/Manifest$permission", "WRITE_EXTERNAL_STORAGE",
             "android.permission.WRITE_EXTERNAL_STORAGE"),
    // UE4.27's two GameActivity inner classes, whose objects kl_jni_ue4.c hands
    // back from AndroidThunkJava_GetInputDeviceInfo and
    // ...LocalNotificationGetLaunchNotification. The engine reads these fields
    // IMMEDIATELY after the call returns, so binding the methods without the
    // fields would only move the abort one line down, from "no host
    // implementation" to "no host value for field" — see the note in
    // kl_jni_ue4.c.
    //
    // InputDeviceInfo: the synthetic Touch pair this runtime presents. Vendor
    // and product are 0, which is what the real GameActivity fills in when its
    // own InputDevice lookup fails — and there is no Android InputDevice behind
    // this one. The name and descriptor are stable strings so the engine's
    // controller mapping sees one device, not a new one per frame.
    KLJ_FINT("com/epicgames/ue4/GameActivity$InputDeviceInfo", "vendorId",     0),
    KLJ_FINT("com/epicgames/ue4/GameActivity$InputDeviceInfo", "productId",    0),
    KLJ_FINT("com/epicgames/ue4/GameActivity$InputDeviceInfo", "controllerId", 0),
    KLJ_FSTR("com/epicgames/ue4/GameActivity$InputDeviceInfo", "name",       "Oculus Touch"),
    KLJ_FSTR("com/epicgames/ue4/GameActivity$InputDeviceInfo", "descriptor", "klepton:touch"),
    // LaunchNotification: this app was launched from the Home View, never from
    // a notification. `used` false is what keeps the game instance out of a
    // notification-handling path with nothing behind it.
    {.cls = "com/epicgames/ue4/GameActivity$LaunchNotification",
     .name = "used", .sig = "Z", .ival = 0},
    KLJ_FSTR("com/epicgames/ue4/GameActivity$LaunchNotification", "event", ""),
    KLJ_FINT("com/epicgames/ue4/GameActivity$LaunchNotification", "fireDate", 0),
    KLJ_FINT("android/view/PointerIcon", "TYPE_DEFAULT", 1000),
    KLJ_FINT("android/view/PointerIcon", "TYPE_ARROW", 1000),
    KLJ_FINT("android/view/PointerIcon", "TYPE_NULL", 0),
    KLJ_FINT("android/view/PointerIcon", "TYPE_CUSTOM", -1),
    {"android/graphics/Bitmap$Config", "ARGB_8888", "Landroid/graphics/Bitmap$Config;", NULL, 0, 0, klj_BitmapConfig_argb8888},
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
    KLJ_FINT("android/media/AudioManager", "GET_DEVICES_INPUTS",  1),
    KLJ_FINT("android/media/AudioManager", "GET_DEVICES_OUTPUTS", 2),
    KLJ_FINT("android/media/AudioManager", "GET_DEVICES_ALL",     3),
    KLJ_FINT("android/media/AudioManager", "STREAM_MUSIC", 3),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUILTIN_SPEAKER", 2),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLUETOOTH_A2DP", 8),
    KLJ_FINT("android/bluetooth/BluetoothAdapter", "STATE_OFF",         10),
    KLJ_FINT("android/bluetooth/BluetoothAdapter", "STATE_TURNING_ON",  11),
    KLJ_FINT("android/bluetooth/BluetoothAdapter", "STATE_ON",          12),
    KLJ_FINT("android/bluetooth/BluetoothAdapter", "STATE_TURNING_OFF", 13),
    // Audio focus and AudioAttributes (AOSP values), read by the API-26
    // requestAudioFocus path in runtime/jni/kl_jni_window.c.
    KLJ_FINT("android/media/AudioManager", "AUDIOFOCUS_GAIN", 1),
    KLJ_FINT("android/media/AudioManager", "AUDIOFOCUS_REQUEST_FAILED", 0),
    KLJ_FINT("android/media/AudioManager", "AUDIOFOCUS_REQUEST_GRANTED", 1),
    KLJ_FINT("android/media/AudioAttributes", "USAGE_MEDIA", 1),
    KLJ_FINT("android/media/AudioAttributes", "USAGE_GAME", 14),
    KLJ_FINT("android/media/AudioAttributes", "CONTENT_TYPE_MUSIC", 2),
    KLJ_FINT("android/media/AudioAttributes", "CONTENT_TYPE_SONIFICATION", 4),
    // MediaFormat colour constants — Unity's AndroidVideoMedia reads these off
    // the decoded video format to set up the output colour conversion. Values
    // are the AOSP MediaFormat definitions.
    KLJ_FINT("android/media/MediaFormat", "COLOR_RANGE_FULL",      1),
    KLJ_FINT("android/media/MediaFormat", "COLOR_RANGE_LIMITED",   2),
    KLJ_FINT("android/media/MediaFormat", "COLOR_STANDARD_BT709",       1),
    KLJ_FINT("android/media/MediaFormat", "COLOR_STANDARD_BT601_PAL",   2),
    KLJ_FINT("android/media/MediaFormat", "COLOR_STANDARD_BT601_NTSC",  4),
    KLJ_FINT("android/media/MediaFormat", "COLOR_STANDARD_BT2020",      6),
    KLJ_FINT("android/media/MediaFormat", "COLOR_TRANSFER_LINEAR",      1),
    KLJ_FINT("android/media/MediaFormat", "COLOR_TRANSFER_SDR_VIDEO",   3),
    KLJ_FINT("android/media/MediaFormat", "COLOR_TRANSFER_ST2084",      6),
    KLJ_FINT("android/media/MediaFormat", "COLOR_TRANSFER_HLG",         7),
    // MediaCodecInfo.CodecCapabilities colour formats — Unity queries these when
    // choosing the decoder output format. AOSP values.
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420Planar",      19),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420SemiPlanar",  21),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420Flexible",    0x7F420888),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatSurface",           0x7F000789),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_QCOM_FormatYUV420SemiPlanar", 0x7FA30C00),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_TI_FormatYUV420PackedSemiPlanar", 0x7F000100),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420PackedSemiPlanar", 39),
    KLJ_FINT("android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420PackedPlanar",     20),

    // AudioDeviceInfo.TYPE_*. A caller that walks getDevices() does not read
    // getType() for its own sake — it reads it to COMPARE, and a comparison
    // needs both sides. Batman: Arkham Shadow dies on exactly that shape: one
    // getDevices(GET_DEVICES_OUTPUTS), one getType() on the device we answer
    // with, and then a GetStaticFieldID for TYPE_BLUETOOTH_A2DP to compare it
    // against — which we had no value for, so the whole process aborted for
    // want of the number 8. The comparison is going to FAIL either way (we
    // report the built-in speaker), and that is the correct outcome; what was
    // wrong was dying instead of answering.
    //
    // So the whole documented family goes in, not just the one name the trace
    // happened to reach. These are fixed platform constants, identical on every
    // Android release that defines them, and a caller classifying an output
    // typically tests several in a row — adding them one abort at a time would
    // cost a failed run per constant. Values from AudioDeviceInfo (API 23+,
    // with the later BLE/eARC additions).
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_UNKNOWN",              0),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUILTIN_EARPIECE",     1),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUILTIN_SPEAKER",      2),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_WIRED_HEADSET",        3),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_WIRED_HEADPHONES",     4),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_LINE_ANALOG",          5),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_LINE_DIGITAL",         6),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLUETOOTH_SCO",        7),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLUETOOTH_A2DP",       8),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_HDMI",                 9),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_HDMI_ARC",            10),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_USB_DEVICE",          11),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_USB_ACCESSORY",       12),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_DOCK",                13),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_FM",                  14),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUILTIN_MIC",         15),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_FM_TUNER",            16),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_TV_TUNER",            17),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_TELEPHONY",           18),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_AUX_LINE",            19),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_IP",                  20),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUS",                 21),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_USB_HEADSET",         22),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_HEARING_AID",         23),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BUILTIN_SPEAKER_SAFE",24),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_REMOTE_SUBMIX",       25),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLE_HEADSET",         26),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLE_SPEAKER",         27),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_HDMI_EARC",           29),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_BLE_BROADCAST",       30),
    KLJ_FINT("android/media/AudioDeviceInfo", "TYPE_DOCK_ANALOG",         31),

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
    KLJ_FINT("android/content/pm/PackageManager", "PERMISSION_DENIED", -1),
    KLJ_FINT("android/content/pm/PackageManager", "GET_ACTIVITIES",            0x00000001),
    KLJ_FINT("android/content/pm/PackageManager", "GET_RECEIVERS",             0x00000002),
    KLJ_FINT("android/content/pm/PackageManager", "GET_SERVICES",              0x00000004),
    KLJ_FINT("android/content/pm/PackageManager", "GET_PROVIDERS",             0x00000008),
    KLJ_FINT("android/content/pm/PackageManager", "GET_INSTRUMENTATION",       0x00000010),
    KLJ_FINT("android/content/pm/PackageManager", "GET_INTENT_FILTERS",        0x00000020),
    KLJ_FINT("android/content/pm/PackageManager", "GET_SIGNATURES",            0x00000040),
    KLJ_FINT("android/content/pm/PackageManager", "GET_META_DATA",             0x00000080),
    KLJ_FINT("android/content/pm/PackageManager", "GET_GIDS",                  0x00000100),
    KLJ_FINT("android/content/pm/PackageManager", "GET_SHARED_LIBRARY_FILES",  0x00000400),
    KLJ_FINT("android/content/pm/PackageManager", "GET_PERMISSIONS",           0x00001000),
    KLJ_FINT("android/content/pm/PackageManager", "GET_CONFIGURATIONS",        0x00004000),
    KLJ_FINT("android/content/pm/PackageManager", "GET_SIGNING_CERTIFICATES",  0x08000000),

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
    // Opening a link or another app (DreamSpace's Spotify button builds
    // `new Intent(); setAction(ACTION_VIEW); setData(Uri.parse(url))` and starts
    // it). Without the constant the run ended there (2026-10-16). What the
    // intent then meets is klj_Context_startActivity.
    KLJ_FSTR("android/content/Intent", "ACTION_VIEW", "android.intent.action.VIEW"),

    KLJ_FSTR("android/os/Environment", "MEDIA_MOUNTED", "mounted"),
    KLJ_FFN("android/net/Uri", "EMPTY", "Landroid/net/Uri;", klj_Uri_EMPTY),
    KLJ_FINT("android/content/res/Configuration", "colorMode", 0),
    KLJ_FINT("android/content/res/Configuration", "densityDpi",   320),
    KLJ_FFLT("android/content/res/Configuration", "fontScale",    1.0),
    KLJ_FINT("android/content/res/Configuration", "orientation",  2),   // ORIENTATION_LANDSCAPE
    KLJ_FINT("android/content/res/Configuration", "keyboard",            1),   // KEYBOARD_NOKEYS
    // The Configuration.KEYBOARD_* constants ZIX reads to compare against
    // `keyboard` above (is a hardware keyboard attached? — no).
    KLJ_FINT("android/content/res/Configuration", "KEYBOARD_UNDEFINED",  0),
    KLJ_FINT("android/content/res/Configuration", "KEYBOARD_NOKEYS",     1),
    KLJ_FINT("android/content/res/Configuration", "KEYBOARD_QWERTY",     2),
    KLJ_FINT("android/content/res/Configuration", "KEYBOARD_12KEY",      3),
    KLJ_FINT("android/content/res/Configuration", "HARDKEYBOARDHIDDEN_UNDEFINED", 0),
    KLJ_FINT("android/content/res/Configuration", "HARDKEYBOARDHIDDEN_NO",        1),
    KLJ_FINT("android/content/res/Configuration", "HARDKEYBOARDHIDDEN_YES",       2),
    KLJ_FINT("android/content/res/Configuration", "hardKeyboardHidden",           2),  // YES (no hw kbd)
    KLJ_FINT("android/content/res/Configuration", "keyboardHidden",      1),   // KEYBOARDHIDDEN_NO
    KLJ_FINT("android/content/res/Configuration", "hardKeyboardHidden",  2),   // HARDKEYBOARDHIDDEN_YES
    KLJ_FINT("android/content/res/Configuration", "navigation",          1),   // NAVIGATION_NONAV
    KLJ_FINT("android/content/res/Configuration", "navigationHidden",    1),   // NAVIGATIONHIDDEN_NO
    KLJ_FINT("android/content/res/Configuration", "touchscreen",         1),   // TOUCHSCREEN_NOTOUCH
    KLJ_FINT("android/content/res/Configuration", "screenLayout",        2),   // SCREENLAYOUT_SIZE_NORMAL
    KLJ_FINT("android/content/res/Configuration", "uiMode",              1),   // UI_MODE_TYPE_NORMAL
    KLJ_FINT("android/content/res/Configuration", "mcc",                 0),
    KLJ_FINT("android/content/res/Configuration", "mnc",                 0),
    KLJ_FINT("android/content/res/Configuration", "seq",                 0),
    KLJ_FINT("android/content/res/Configuration", "smallestScreenWidthDp", 600),
    KLJ_FINT("android/content/res/Configuration", "screenWidthDp",       1024),
    KLJ_FINT("android/content/res/Configuration", "screenHeightDp",      768),

    // ApplicationInfo is read field-by-field, and these depend on runtime
    // configuration rather than being compile-time constants.
    KLJ_FFN("android/content/pm/ApplicationInfo", "sourceDir",        "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "publicSourceDir",  "Ljava/lang/String;", klj_appinfo_sourceDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "nativeLibraryDir", "Ljava/lang/String;", klj_appinfo_nativeLibraryDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "dataDir",          "Ljava/lang/String;", klj_appinfo_dataDir),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitSourceDirs",       "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FFN("android/content/pm/ApplicationInfo", "splitPublicSourceDirs", "[Ljava/lang/String;", klj_appinfo_splitSourceDirs),
    KLJ_FINT("android/content/pm/ApplicationInfo", "flags", 0),
    // Declared SDK bounds, read field-by-field like the rest of ApplicationInfo.
    // The OS already enforced minSdk at install, so these are informational; the
    // real values come from the guest's apktool.yml (e.g. 32 / 34).
    KLJ_FINT("android/content/pm/ApplicationInfo", "minSdkVersion", 32),
    KLJ_FINT("android/content/pm/ApplicationInfo", "targetSdkVersion", 34),

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
    KLJ_FFN("android/content/pm/PackageInfo", "reqFeatures",
            "[Landroid/content/pm/FeatureInfo;", klj_PackageInfo_reqFeatures),

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
    // The other direction: the activity's handle on the player it constructed.
    // UnityPlayerActivity.smali declares
    //   .field protected mUnityPlayer:Lcom/unity3d/player/UnityPlayer;
    // and sets it in onCreate. A C# plugin reaching the player does
    //   UnityPlayer.currentActivity.Get<AndroidJavaObject>("mUnityPlayer")
    // which arrives here through ReflectionHelper.getFieldID with the ERASED
    // Ljava/lang/Object; — the same two-spellings-of-one-field shape as
    // currentActivity above, so both are listed and both call one function.
    // Paint the Town Red stops here on its first guest frame without it, one
    // read after registerInputDeviceListener.
    KLJ_FFN("com/unity3d/player/UnityPlayerActivity", "mUnityPlayer",
            "Lcom/unity3d/player/UnityPlayer;", klj_mUnityPlayer_field),
    KLJ_FFN("com/unity3d/player/UnityPlayerActivity", "mUnityPlayer",
            "Ljava/lang/Object;", klj_mUnityPlayer_field),
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
    // ...and its preview twin, which Unreal 5.3 (Pinball FX VR) reads right
    // after SDK_INT. It is 0 on every SHIPPED Android build and non-zero only
    // on a developer preview, which this device is declared not to be; the
    // engine adds it to SDK_INT to get an effective API level, so 0 is both the
    // truthful answer and the one that leaves SDK_INT meaning what it says.
    KLJ_FINT("android/os/Build$VERSION", "PREVIEW_SDK_INT", 0),
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

    // Unity's internetReachability compares getActiveNetworkInfo().getType()
    // against these two to decide LocalAreaNetwork vs CarrierDataNetwork.
    // android-34 android.jar values, like the MediaRouter block above.
    KLJ_FINT("android/net/ConnectivityManager", "TYPE_MOBILE", 0),
    KLJ_FINT("android/net/ConnectivityManager", "TYPE_WIFI",   1),

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
    klj_bind_electra,
    klj_bind_fmod,
    klj_bind_jkxr,
    klj_bind_haptics,
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
    kl_key_create_or_die(&g_env_key, free, "the per-thread JNIEnv");
    // The defaults are relative literals; the setters resolve what they are
    // given, so this covers the case where nothing set them.
    g_assets_dir     = klj_abspath(g_assets_dir);
    (void)kl_jni_files_dir();   // resolve now, so the report below prints it
    (void)kl_jni_apk_path();    // resolves KL_APK / the default, absolutised
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
        if (!e) abort();
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
    ENV(Throw,                klj_Throw);
    ENV(ThrowNew,             klj_ThrowNew);
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
    ENV(IsAssignableFrom,     klj_IsAssignableFrom);
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
    ENV(GetPrimitiveArrayCritical, klj_GetPrimitiveArrayCritical);
    ENV(ReleasePrimitiveArrayCritical, klj_ReleasePrimitiveArrayCritical);
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

// ---------------------------------------------- the object-lifetime gate
// `make jniobj`, in `make check`. Milliseconds, no guest, no window.
//
// Two facts about the object model, both learned the expensive way — each was
// a boot that died hundreds of lines from the binding that actually caused it:
//
//  1. A binding that CACHES its answer in a host static must pin it. The
//     object is allocated inside whatever local frame the first caller had
//     open; that frame's pop retires the slot, the static keeps the pointer,
//     and the next caller is handed a jobject we no longer recognise. Paint
//     the Town Red VR's boot aborted in GetObjectClass on exactly that
//     pointer — the int[] from InputDevice.getDeviceIds(), four calls after it
//     was built.
//
//  2. Unity's C# side asks for these methods with ERASED spellings, and the
//     erased call must produce the same tagged object the exact one does.
//     `AndroidJavaObject.CallStatic<AndroidJavaObject>("getDeviceIds")` spells
//     the return `Ljava/lang/Object;`; the same call with a null or
//     object-typed argument erases that end too. All three spellings resolve
//     to one binding, so all three must come back with the same identity.
//
// The subject is the InputDevice family because it is where both defects were
// found and because it exercises every return shape at once: a primitive
// array, an object, a string, an int and a boolean.
int kl_jni_object_lifetime_selftest(FILE *out) {
    int fails = 0, checks = 0;
#define OBJ_CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fprintf(out, "  FAIL: " __VA_ARGS__); fprintf(out, "\n"); fails++; } \
    } while (0)

    void *env = kl_jni_env();
    void *cls = klj_class_object("android/view/InputDevice");

    // ---- 1. the cached int[] outlives the frame it was built in ----
    void *mid_exact = klj_want(cls, "getDeviceIds", "()[I", 'M');
    klj_PushLocalFrame(env, 16);
    klj_val r1 = klj_call_a(env, cls, mid_exact, NULL, 'L');
    klj_PopLocalFrame(env, NULL);
    OBJ_CHECK(klj_as_object(r1.l) != NULL,
              "the int[] from getDeviceIds()[I did not survive the local frame it "
              "was built in — a host static holding a jobject must go through "
              "klj_host_cache, or the next caller gets a retired slot");

    // ---- 2. the ERASED return answers with the SAME object ----
    void *mid_er = klj_want(cls, "getDeviceIds", "()Ljava/lang/Object;", 'M');
    klj_val r2 = klj_call_a(env, cls, mid_er, NULL, 'L');
    OBJ_CHECK(klj_as_object(r2.l) != NULL,
              "getDeviceIds()Ljava/lang/Object; — the erased spelling Unity's C# "
              "uses — did not return a tagged jobject");
    OBJ_CHECK(r2.l == r1.l,
              "the erased and exact spellings of getDeviceIds returned DIFFERENT "
              "objects; Unity keys its joystick slots on that identity");

    // ...and it is an int[], which is what GetObjectClass has to be able to say:
    // Unity builds its AndroidJavaObject wrapper out of exactly this answer.
    if (klj_as_object(r2.l)) {
        void *k = klj_GetObjectClass(env, r2.l);
        const char *kn = klj_class_name(k);
        OBJ_CHECK(strcmp(kn, "[I") == 0,
                  "GetObjectClass on the int[] said \"%s\", not \"[I\"", kn);
        OBJ_CHECK(klj_GetArrayLength(env, r2.l) == 2,
                  "the device-id array is %d long, not 2 (the two Touch controllers)",
                  (int)klj_GetArrayLength(env, r2.l));
        int32_t ids[2] = {0, 0};
        klj_GetIntArrayRegion(env, r2.l, 0, 2, ids);
        OBJ_CHECK(ids[0] == 8 && ids[1] == 9,
                  "the device ids read back as [%d, %d], not [8, 9] — the ids are "
                  "stable on purpose", ids[0], ids[1]);
    }

    // ---- 3. getDevice(int) through the erased return, across a frame ----
    void *mid_dev = klj_want(cls, "getDevice", "(I)Ljava/lang/Object;", 'M');
    klj_jvalue av[2];
    av[0].i = 8;
    klj_PushLocalFrame(env, 16);
    klj_val dev = klj_call_a(env, cls, mid_dev, av, 'L');
    klj_PopLocalFrame(env, NULL);
    OBJ_CHECK(klj_as_object(dev.l) != NULL,
              "getDevice(8) through the erased return did not survive its frame");

    if (klj_as_object(dev.l)) {
        const char *dn = klj_class_name(klj_GetObjectClass(env, dev.l));
        OBJ_CHECK(strcmp(dn, "android/view/InputDevice") == 0,
                  "getDevice(8) answered an object of class \"%s\"", dn);

        // Everything Rewired asks the device afterwards, in the spellings C#
        // produces: a String return erased to Object, and the primitives, which
        // cannot be erased and so must still match exactly.
        void *mid_name = klj_want(cls, "getName", "()Ljava/lang/Object;", 'm');
        klj_val nm = klj_call_a(env, dev.l, mid_name, NULL, 'L');
        const char *nms = klj_str(nm.l);
        OBJ_CHECK(nms && strcmp(nms, "Oculus Touch Controller - Left") == 0,
                  "getName() erased to Object answered \"%s\"", nms ? nms : "(null)");

        void *mid_desc = klj_want(cls, "getDescriptor", "()Ljava/lang/Object;", 'm');
        klj_val ds = klj_call_a(env, dev.l, mid_desc, NULL, 'L');
        const char *dss = klj_str(ds.l);
        OBJ_CHECK(dss && strcmp(dss, "klepton-touch-l") == 0,
                  "getDescriptor() erased to Object answered \"%s\"", dss ? dss : "(null)");

        void *mid_pid = klj_want(cls, "getProductId", "()I", 'm');
        void *mid_vid = klj_want(cls, "getVendorId",  "()I", 'm');
        void *mid_vrt = klj_want(cls, "isVirtual",    "()Z", 'm');
        OBJ_CHECK((int32_t)klj_call_a(env, dev.l, mid_pid, NULL, 'I').j == 0,
                  "getProductId() is not 0 — the Touch controllers are not USB devices");
        OBJ_CHECK((int32_t)klj_call_a(env, dev.l, mid_vid, NULL, 'I').j == 0,
                  "getVendorId() is not 0");
        OBJ_CHECK((klj_call_a(env, dev.l, mid_vrt, NULL, 'Z').j & 1) == 0,
                  "isVirtual() is true — these are physical controllers");

        // hasKeys, with BOTH ends erased: the argument spelled Ljava/lang/Object;
        // (an int[] held in an `object`) and the return spelled the same. That is
        // the one call needing the argument tier and the return tier at once.
        void *keys = klj_new_array('I', NULL, 3);
        ((int32_t *)klj_arr(keys)->data)[0] = 96;    // BUTTON_A
        ((int32_t *)klj_arr(keys)->data)[1] = 97;    // BUTTON_B
        ((int32_t *)klj_arr(keys)->data)[2] = 99;    // BUTTON_X
        void *mid_keys = klj_want(cls, "hasKeys",
                                  "(Ljava/lang/Object;)Ljava/lang/Object;", 'm');
        av[0].l = keys;
        klj_val hk = klj_call_a(env, dev.l, mid_keys, av, 'L');
        OBJ_CHECK(klj_as_object(hk.l) != NULL,
                  "hasKeys() with the argument AND the return erased to Object did "
                  "not resolve — the loose tier wants identical arguments and the "
                  "assignability tier wanted an identical return, and this call "
                  "needs both relaxations at once");
        if (klj_as_object(hk.l)) {
            const char *hn = klj_class_name(klj_GetObjectClass(env, hk.l));
            OBJ_CHECK(strcmp(hn, "[Z") == 0,
                      "hasKeys() answered an object of class \"%s\", not \"[Z\"", hn);
            OBJ_CHECK(klj_GetArrayLength(env, hk.l) == 3,
                      "hasKeys() answered %d flags for 3 keys — Rewired indexes it "
                      "positionally", (int)klj_GetArrayLength(env, hk.l));
            uint8_t flags[3] = {1, 1, 1};
            klj_GetBooleanArrayRegion(env, hk.l, 0, 3, flags);
            OBJ_CHECK(!flags[0] && !flags[1] && !flags[2],
                      "hasKeys() claimed a Touch controller carries an Android "
                      "keymap; Rewired reads that as a DualShock 4");
        }
    }

    // ---- 4. the lookup the guest made with NO signature ----
    //
    // Unity's AndroidJNIHelper.GetMethodID has an optional `signature`
    // parameter, and bHaptics' Unity wrapper omits it for five of its methods.
    // The id must still come back carrying the REAL signature, or the call it
    // is used for has no return kind and no argument list.
    void *bh = klj_class_object("com/bhaptics/bhapticsunity/BhapticsManagerWrapper");

    void *mid_nosig = klj_want(bh, "refreshPairing", "", 'm');
    OBJ_CHECK(strcmp(((klj_wanted *)mid_nosig)->sig, "()V") == 0,
              "refreshPairing asked with NO signature was issued with sig \"%s\", "
              "not \"()V\" — the call would decode no arguments and report return "
              "kind '?'", ((klj_wanted *)mid_nosig)->sig);

    // ...and it interns to the SAME id as the fully-spelled lookup, so a guest
    // that asks both ways does not get two entries for one method.
    OBJ_CHECK(klj_want(bh, "refreshPairing", "()V", 'm') == mid_nosig,
              "the signed and unsigned spellings of refreshPairing were issued as "
              "two different jmethodIDs");

    // The call itself: through the 'V' slot, which is the one that warned.
    klj_call_a(env, bh, mid_nosig, NULL, 'V');

    void *mid_user = klj_want(bh, "isBhapticsUser", "", 'm');
    OBJ_CHECK(strcmp(((klj_wanted *)mid_user)->sig, "()Z") == 0,
              "isBhapticsUser asked with NO signature was issued with sig \"%s\"",
              ((klj_wanted *)mid_user)->sig);
    OBJ_CHECK((klj_call_a(env, bh, mid_user, NULL, 'Z').j & 1) == 0,
              "isBhapticsUser() answered true — there is no bHaptics hardware here");

    // An overloaded name is REFUSED rather than guessed: isPlayingByEventId is
    // declared for both int and String, and marshalling one as the other is how
    // a name-only lookup turns into a wrong answer instead of a stop.
    void *mid_amb = klj_want(bh, "isPlayingByEventId", "", 'm');
    OBJ_CHECK(((klj_wanted *)mid_amb)->sig[0] == 0,
              "isPlayingByEventId asked with NO signature resolved to \"%s\"; it is "
              "overloaded and must be refused, not picked by table order",
              ((klj_wanted *)mid_amb)->sig);

    // A name nothing implements resolves to nothing, and stays empty.
    void *mid_none = klj_want(bh, "stopInt", "", 'm');
    OBJ_CHECK(((klj_wanted *)mid_none)->sig[0] == 0,
              "an unbound name asked with NO signature was given sig \"%s\"",
              ((klj_wanted *)mid_none)->sig);

    // ---- 5. an argument spelled as the guest's own activity class ----
    //
    // Unity's reflection helper spells a parameter as the class of the object
    // it passes, dotted, so WebSettings.getDefaultUserAgent(Context) is asked
    // for with UnityPlayerActivity in the slot. The row is declared as Java
    // declares it and is reached through the assignability tier — and the game
    // that asks (Real VR Fishing, fix 37) is one nobody here can run, so this
    // is where the spelling is held to. Resolved before it is called: a call
    // that resolves to nothing ends the process instead of failing a check.
    static const char ua_sig[] = "(Lcom.unity3d.player.UnityPlayerActivity;)Ljava/lang/String;";
    int ua_bound = klj_resolve_binding("android/webkit/WebSettings",
                                       "getDefaultUserAgent", ua_sig) != NULL;
    OBJ_CHECK(ua_bound, "WebSettings.getDefaultUserAgent%s resolves to no binding", ua_sig);
    if (ua_bound) {
        void *ws = klj_class_object("android/webkit/WebSettings");
        klj_jvalue ua_arg[1] = {{0}};
        const char *ua = klj_str(klj_call_a(env, ws, klj_want(ws, "getDefaultUserAgent", ua_sig, 'M'),
                                            ua_arg, 'L').l);
        const char *model = kl_jni_build_string("MODEL");
        OBJ_CHECK(ua && model && strstr(ua, model),
                  "getDefaultUserAgent answered \"%s\", which does not name the "
                  "model Build.MODEL does (\"%s\")", ua ? ua : "(null)", model ? model : "(null)");
    }

    fprintf(out, "  %d checks, %d failed\n", checks, fails);
#undef OBJ_CHECK
    return fails == 0;
}
