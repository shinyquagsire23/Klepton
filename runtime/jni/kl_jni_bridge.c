// JNIBridge and Unity's ReflectionHelper
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
// The strings are the CALLER's, and the two callers disagree about who owns
// them — getMethodID strdups guest jstrings, klj_proxy_invoke passes borrowed
// interned names. So the destructor is attached at the owning call site rather
// than here, which is the whole reason ownership is recorded per object.
static void *klj_new_method(const char *cls, const char *name, const char *sig) {
    klj_method_obj *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cls = cls; m->name = name; m->sig = sig;
    return klj_new_object_data(KLJ_CLASS_METHOD, m);
}

static void klj_method_free(void *p) {
    klj_method_obj *m = p;
    if (!m) return;
    free((void *)m->cls); free((void *)m->name); free((void *)m->sig);
    free(m);
}

static void klj_field_free(void *p) {
    klj_field_obj *f = p;
    if (!f) return;
    free((void *)f->cls); free((void *)f->name); free((void *)f->sig);
    free(f);
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
// The strings are copied, not aliased: they arrive as guest jstrings, whose
// storage the guest reuses the moment this returns.
static void *klj_new_field(const char *cls, const char *name, const char *sig, int is_static) {
    klj_field_obj *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->cls = cls ? strdup(cls) : NULL;
    f->name = name ? strdup(name) : NULL;
    f->sig = sig ? strdup(sig) : NULL;
    f->is_static = is_static;
    return klj_own(klj_new_object_data(KLJ_CLASS_FIELD, f), klj_field_free);
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
    void *m = klj_own(klj_new_method(strdup(cls), name ? strdup(name) : NULL,
                                     sig ? strdup(sig) : NULL), klj_method_free);
    klj_object *o = klj_as_object(m);
    if (o && o->data) ((klj_method_obj *)o->data)->is_static = is_static;
    return (klj_val){.l = m};
}

// ...and the constructor half. A Constructor IS a Method here — the same record
// with the name `<init>` — because that is exactly what it is on the JNI side:
// FromReflectedMethod turns it into a jmethodID, and klj_call already treats a
// method named `<init>` as returning the new object rather than the 'V' its
// signature claims (see klj_NewObjectV). So there is no second object kind and
// no second conversion path, which is the point.
//
// Unity passes the signature already built, argument types only; the class is
// the reflected one. `is_static` is false by construction.
static klj_val klj_ReflectionHelper_getConstructorID(void *env, void *self,
                                                     const klj_val *a, int n) {
    (void)env; (void)self;
    const char *cls = n > 0 ? klj_class_name(a[0].l) : NULL;
    const char *sig = n > 1 ? klj_str(a[1].l) : NULL;
    if (!cls) {
        KLJ_LOG("ReflectionHelper.getConstructorID with no class -> null");
        return (klj_val){.l = NULL};
    }
    KLJ_LOG("ReflectionHelper.getConstructorID %s.<init>%s", cls, sig ? sig : "()V");
    return (klj_val){.l = klj_own(klj_new_method(strdup(cls), strdup("<init>"),
                                                 strdup(sig ? sig : "()V")),
                                  klj_method_free)};
}

// Unity's own door to the same proxy JNIBridge.newInterfaceProxy builds. The
// arguments are shaped differently — the UnityPlayer instance, the native
// pointer, and ONE interface class rather than an array — but the object handed
// back has to be the same kind, because klj_proxy_invoke is what dispatches
// every callback through it and it knows exactly one representation.
static klj_val klj_ReflectionHelper_newProxyInstance(void *env, void *clazz,
                                                     const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_proxy *p = calloc(1, sizeof *p);
    p->native_ptr = n > 1 ? (int64_t)a[1].j : 0;
    // Wrapped in a one-element array so the two doors' proxies are identical
    // down to the field, rather than alike: klj_proxy_invoke and the report
    // both read `classes` as an object array.
    void *ifaces = klj_new_array('L', "java/lang/Class", 1);
    if (n > 2) ((void **)klj_arr(ifaces)->data)[0] = a[2].l;
    klj_as_object(ifaces)->pinned = 1;
    p->classes = ifaces;
    KLJ_LOG("ReflectionHelper.newProxyInstance: implements %s (native 0x%llx)",
            n > 2 ? klj_class_name(a[2].l) : "(none)",
            (unsigned long long)p->native_ptr);

    void *obj = kl_jni_new_object(KLJ_CLASS_PROXY);
    klj_as_object(obj)->data = p;
    klj_as_object(obj)->pinned = 1;   // guest-held long-term via native_ptr
    return (klj_val){.l = obj};
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
 void *klj_FromReflectedField(void *env, void *field) {
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
 void *klj_proxy_invoke(void *proxy, const char *iface,
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
    if (p->disabled) {
        KLJ_LOG("proxy 0x%llx is disabled — not invoking %s.%s",
                (unsigned long long)p->native_ptr, iface, name);
        return NULL;
    }
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
// Delivery, and then the message dies. obtainMessage pins it because it is
// queued and outlives the frame that made it — and a pinned object is skipped
// by klj_retire_object_locked, so the frame pop that would have reclaimed the
// slot did nothing and nothing else ever would: a guest that obtains messages
// repeatedly walks the pool to "object pool exhausted", which is an abort
// rather than an OOM and is a different bug wearing the same clothes.
//
// It is retired AFTER the callback, not before: retiring clears the magic, so
// an early unpin-and-retire would hand the guest's own handleMessage a Message
// that no longer answers klj_as_message.
static void klj_deliver_message_1(void *message);

void klj_deliver_message(void *message) {
    klj_deliver_message_1(message);
    klj_object *o = klj_as_object(message);
    if (!o) return;
    pthread_mutex_lock(&g_lock);
    o->pinned = 0;
    klj_retire_object_locked(o);
    pthread_mutex_unlock(&g_lock);
}

static void klj_deliver_message_1(void *message) {
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

// ...and an int, for the same reason: a dialog button id reaches the guest's
// listener as an Integer in that Object[].
void *klj_box_int(int32_t v) {
    klj_pref *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->kind = 'I';
    e->ival = v;
    void *obj = kl_jni_new_object("java/lang/Integer");
    klj_as_object(obj)->data = e;
    return obj;
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
    // Whoever runs the posted work IS the UI thread — see klj_on_ui_thread.
    g_ui_thread = pthread_self();
    g_ui_thread_known = 1;
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
    // The soft input's reports, which on Android are queueGLThreadEvent()s
    // rather than UI-thread posts — a different queue with the same drain
    // point, because here the thread that pumps this IS the render thread (it
    // calls nativeRender on the line above this call, in both Unity drivers).
    // Folded in rather than exported so a driver cannot pump one queue and not
    // the other; the count stays the UI tasks' so the drivers' "drained N
    // posted tasks" keeps meaning what it did.
    klj_drain_soft_input();
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

const klj_binding klj_bind_bridge[] = {

    {"com/unity3d/player/ReflectionHelper", "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", klj_ReflectionHelper_getFieldID},
    {"com/unity3d/player/ReflectionHelper", "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", klj_ReflectionHelper_getMethodID},
    {"com/unity3d/player/ReflectionHelper", "getConstructorID", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;", klj_ReflectionHelper_getConstructorID},
    {"com/unity3d/player/ReflectionHelper", "newProxyInstance", "(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;", klj_ReflectionHelper_newProxyInstance},
    {"com/unity3d/player/ReflectionHelper", "getFieldSignature", "(Ljava/lang/reflect/Field;)Ljava/lang/String;", klj_ReflectionHelper_getFieldSignature},
    {"java/lang/reflect/Field", "getDeclaringClass", "()Ljava/lang/Class;", klj_Field_getDeclaringClass},
    {"java/lang/reflect/Method", "getName", "()Ljava/lang/String;", klj_Method_getName},
    {"java/lang/reflect/Method", "getDeclaringClass", "()Ljava/lang/Class;", klj_Method_getDeclaringClass},
    {"android/view/View", "setSystemUiVisibility", "(I)V", klj_View_setSystemUiVisibility},
    {"android/view/View", "getSystemUiVisibility", "()I", klj_View_getSystemUiVisibility},
    {"android/view/View", "setOnSystemUiVisibilityChangeListener",
     "(Landroid/view/View$OnSystemUiVisibilityChangeListener;)V", klj_View_setSysUiListener},
    {0}
};
