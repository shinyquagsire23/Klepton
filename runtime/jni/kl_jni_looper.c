// the main-thread queue and the frame clock: Handler/Looper,
// MessageQueue, Message, HandlerThread, Choreographer, Process
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
#include "kl_mediandk.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include "kl_jni_int.h"
#include "guest/kl_driver.h"

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
klj_ui_task g_ui_tasks[KLJ_MAX_UI_TASKS];
unsigned g_ui_task_n;

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

// Forward-declared: the inline path below calls into the guest, and the JNIBridge
// machinery that does it lives further down beside the other host->guest calls.

// ...and here is the "unless already on that thread" half, which was missing and
// is not a nicety: `Activity.runOnUiThread` runs the action IMMEDIATELY and
// inline when the caller is already the UI thread, and a guest is allowed to
// depend on that having happened by the time the call returns.
//
// BONELAB does. Its `gles-api-check` warning is posted this way and then WAITED
// ON — the poster blocks on a condition variable the dialog's own callback
// signals — so queuing it deadlocked the process against itself: the only
// thread that drains the queue was the thread inside the wait
// (`__psynch_cvwait` under libunity, with the task still pending). Nothing in
// the log named a dialog; it read as a hung engine.
//
// "Am I the UI thread" is answered by WHO DRAINS THE QUEUE, which is a
// definition rather than a guess: the UI thread is the thread that runs posted
// work, so the thread inside kl_jni_drain_ui_tasks() is that thread and no
// other. A recorded id from the driver would be a second opinion about the same
// fact, and the two would eventually differ (they already do across drivers —
// kl_slink prepares a looper on its activity thread, the Unity path never
// does, so a looper test answers false for exactly the thread that matters).
//
// A thread that has never drained still queues, which is what Android does for
// a worker thread.
 pthread_t g_ui_thread;
 int       g_ui_thread_known;

static int klj_on_ui_thread(void) {
    return (g_ui_thread_known && pthread_equal(g_ui_thread, pthread_self()))
        || kl_ndk_thread_has_looper();
}

// Mark the CALLING thread as the activity/UI thread and give it a native looper.
// Android's main thread always has one (Looper.prepareMainLooper before onCreate),
// and the Unity path never prepared it here — so newer Unity guests failed "Couldn't retrieve native ALooper for UI thread" and spun
// on graphics-context setup until SIGILL. The driver calls this on the thread
// that runs UnityPlayer.initJni, before Unity's native code first asks.
void kl_jni_mark_ui_thread(void) {
    g_ui_thread = pthread_self();
    g_ui_thread_known = 1;
    kl_ndk_prepare_looper();
}

static klj_val klj_Activity_runOnUiThread(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    void *r = n > 0 ? a[0].l : NULL;
    if (r && klj_on_ui_thread()) {
        KLJ_LOG("runOnUiThread: already on the UI thread — running it inline");
        klj_proxy_invoke(r, "java/lang/Runnable", "run", "()V", NULL);
        return (klj_val){0};
    }
    klj_ui_enqueue("runOnUiThread", r, 0);
    return (klj_val){0};
}

// The main Looper is an identity, not a mechanism: Unity holds it to build a
// Handler and to ask whether it is already on that thread.
static klj_val klj_Looper_getMainLooper(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *main_looper;
    return klj_singleton("android/os/Looper", &main_looper);
}

// Looper.myLooper() — the CALLING thread's looper, or null if it has none.
// The distinction is the whole point of the call: a guest asks it to find out
// whether it may post work from here, and a runtime that always answers
// non-null tells every worker thread it is a UI thread.
//
// The native side already knows: ALooper_forThread() is the same question, and
// kl_ndk_prepare_looper is what puts one on the thread that runs the activity.
// So this defers to that rather than keeping a second idea of which threads are
// loopered — two answers to one question is how they come to disagree.
//
// The looper it names is the main one, which is right for the thread we prepare
// and would be wrong for a HandlerThread asking about itself. Nothing does that
// yet; when something does, this needs the per-thread object, not the singleton.
// Looper.getQueue() — the MessageQueue behind a Looper. Like the Looper itself
// this is an identity rather than a mechanism: the queue is ours (klj_looper's
// ring, or the native looper's), and nothing has yet called a method ON the
// MessageQueue — the guest holds it, which is what addIdleHandler and
// isIdle would need and neither has been reached.
//
// So it is a singleton per Looper object, and the moment something does call a
// method on it, that method is where the real queue gets attached.
static klj_val klj_Looper_getQueue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    static void *queue;
    (void)o;
    return klj_singleton("android/os/MessageQueue", &queue);
}

// Looper.prepare() — give THIS thread a looper. On Android it is the first half
// of the two-line idiom every worker thread that wants a message queue writes
// (prepare, then loop). Both halves are ours: the native looper is the same
// object, so this is kl_ndk_prepare_looper by another name.
//
// Android throws RuntimeException on a second prepare for the same thread, and
// kl_ALooper_prepare is idempotent instead. That divergence is deliberate: the
// throw exists to catch a programming error, and we have no exception to throw
// that the guest would survive.
static klj_val klj_Looper_prepare(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    kl_ndk_prepare_looper();
    return (klj_val){0};
}

static void *klj_looper_thread_object(void);
static klj_val klj_Looper_myLooper(void *env, void *self, const klj_val *a, int n) {
    // A HandlerThread's own thread first: its looper is not the main one.
    void *mine = klj_looper_thread_object();
    if (mine) return (klj_val){.l = mine};
    if (!kl_ndk_thread_has_looper()) return (klj_val){.l = NULL};
    return klj_Looper_getMainLooper(env, self, a, n);
}
// new Handler() binds to the calling thread's Looper; new Handler(looper) to the
// one given. We have a single queue, so the Looper is recorded and not acted on.
// The Callback form keeps the callback: sendToTarget() has to find it again, and
// it is the only thing that gives a Message any meaning.

static klj_val klj_Handler_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_handler *h = calloc(1, sizeof *h);
    if (h) {
        h->looper   = n > 0 ? a[0].l : NULL;
        h->callback = n > 1 ? a[1].l : NULL;
        // A Handler keeps its Looper alive, as in Java. The one a HandlerThread's
        // getLooper() hands out is a LOCAL the guest drops once the Handler is
        // built; unpinned, it was retired at the next frame pop and its slot
        // recycled, klj_looper_of() then found no looper, and a message sent
        // later went to the main queue instead of the HandlerThread. LES MILLS
        // BODYCOMBAT's level-start clip (Unity VideoPlayer, sent after its
        // decoder was set up) showed nothing on device that way, 2026-10-15,
        // while the boot clips — sent straight after the Handler was made — played.
        // Moving a message to another thread is a change any game could see,
        // so KL_JNI_PIN_LOOPER=0|1 is on by default for that title only.
        static int pin = -1;
        if (pin < 0) {
            const char *t = kl_driver_target_name();
            pin = kl_env_on("KL_JNI_PIN_LOOPER", t && strcmp(t, "x4015163475201433") == 0);
        }
        klj_object *lo = klj_as_object(h->looper);
        if (pin && lo && lo->data && strcmp(lo->cls, "android/os/Looper") == 0)
            kl_jni_pin_object(h->looper);
    }
    klj_as_object(obj)->data = h;
    if (h && h->callback) {
        klj_object *cb = klj_as_object(h->callback);
        KLJ_LOG("new Handler(looper, callback=%s)", cb ? cb->cls : "(untagged)");
    }
    return (klj_val){.l = obj};
}

 klj_handler *klj_as_handler(void *obj) {
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
    int             finished, joined;      // the thread has left its loop / been reaped
    int             busy;                  // a message is being delivered right now
    void           *q[KLJ_MAX_LOOPER_MSGS];
    unsigned        head, tail, count;
} klj_looper;


// Guest code runs on this thread, so kl_thread_init() before the first delivery is
// mandatory — without it the stack-protector prologue reads an
// empty TSD slot and the guest dies a long way from here.
// Which looper the calling thread IS, when it is one of these: what
// Looper.myLooper() answers on it. A callback that ends its own thread asks
// for exactly that — myLooper().quit() from inside handleMessage.
static _Thread_local void *g_tls_looper;    // klj_looper *, typed below

static void *klj_looper_thread(void *arg) {
    klj_looper *lp = arg;
    kl_thread_init();
    g_tls_looper = lp;
    pthread_mutex_lock(&lp->lock);
    while (lp->running) {
        if (!lp->count) { pthread_cond_wait(&lp->wake, &lp->lock); continue; }
        void *msg = lp->q[lp->head];
        lp->head  = (lp->head + 1) % KLJ_MAX_LOOPER_MSGS;
        lp->count--;
        lp->busy = 1;
        pthread_mutex_unlock(&lp->lock);
        klj_deliver_message(msg);          // outside the lock: it runs guest code
        pthread_mutex_lock(&lp->lock);
        lp->busy = 0;
    }
    lp->finished = 1;
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

// The Looper object of the HandlerThread the caller is running on, or NULL on
// any other thread. One per thread, made on first use.
static void *klj_looper_thread_object(void) {
    static _Thread_local void *obj;
    if (!g_tls_looper) return NULL;
    if (!obj) {
        obj = kl_jni_new_object("android/os/Looper");
        klj_as_object(obj)->data = g_tls_looper;
        // Kept for the thread's life: a local frame popping must not retire it.
        kl_jni_pin_object(obj);
    }
    return obj;
}

// Looper.quit() — stop the loop on the looper this object names. For a
// HandlerThread's looper that is a real thread to shut down; for the main one
// there is nothing to stop, because our main "loop" is the host's pump and it
// ends when the run does.
static void klj_mq_quit(void);
static klj_val klj_Looper_quit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_looper_of(self);
    if (!lp) {
        KLJ_LOG("Looper.quit() on the main looper — releasing anything blocked in "
                "MessageQueue.next()");
        klj_mq_quit();
        return (klj_val){0};
    }
    pthread_mutex_lock(&lp->lock);
    lp->running = 0;
    pthread_cond_broadcast(&lp->wake);
    pthread_mutex_unlock(&lp->lock);
    return (klj_val){0};
}

// ---- android.os.MessageQueue.next() ----------------------------------------
//
// A blocking pop off the Looper's queue, and the first thing in this shim to
// call one. Who calls it is worth writing down, because it is not obvious and it
// is the answer to "how does the in-headset UI talk BACK to the guest".
//
// libvrlink_scene cannot subclass WebMessagePort$WebMessageCallback — it is all
// native, there is no Java of its own in the APK — so it never registers one:
// WebView::UIThread_SetupWebView passes a NULL callback and a Handler bound to
// the WebView thread's Looper (+0x149d1c). Then that thread runs its OWN loop
// (WebView::WebViewThread +0x14a9b0): queue.next(), and for each Message it
// reads the payload straight out of the Message's fields, skipping the three
// framework target classes it knows by name. It is intercepting the framework's
// own delivery instead of receiving it. Clever, and it means a page->native
// message here is a Message on this queue, not a callback we can invoke.
//
// Nothing posts one. Java Messages in this shim go to a HandlerThread's own
// queue (klj_looper above) and Runnables go through kl_jni_drain_ui_tasks, so
// this queue is genuinely, correctly empty — and an empty queue is a wait, not
// a NULL. NULL means "the looper quit", and returning it would tell the guest to
// tear its WebView thread down. So this blocks, exactly as Android's does, and
// says so once so that a future stall here has a name instead of being a silent
// parked thread.
#define KLJ_MQ_MAX 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mq_wake = PTHREAD_COND_INITIALIZER;
static void     *g_mq_q[KLJ_MQ_MAX];
static unsigned  g_mq_head, g_mq_count;   // push goes at (head + count) % KLJ_MQ_MAX
static int       g_mq_quit, g_mq_said;

static void klj_mq_quit(void) {
    pthread_mutex_lock(&g_mq_lock);
    g_mq_quit = 1;
    pthread_cond_broadcast(&g_mq_wake);
    pthread_mutex_unlock(&g_mq_lock);
}

static klj_val klj_MessageQueue_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    pthread_mutex_lock(&g_mq_lock);
    for (;;) {
        if (g_mq_count) {
            void *msg = g_mq_q[g_mq_head];
            g_mq_head = (g_mq_head + 1) % KLJ_MQ_MAX;
            g_mq_count--;
            pthread_mutex_unlock(&g_mq_lock);
            return (klj_val){.l = msg};
        }
        if (g_mq_quit) {
            pthread_mutex_unlock(&g_mq_lock);
            return (klj_val){.l = NULL};
        }
        if (!g_mq_said) {
            g_mq_said = 1;
            KLJ_LOG("MessageQueue.next() — blocking on an empty queue; nothing "
                    "posts Java Messages here (this is the WebView's page->native "
                    "pump, and no page is talking)");
        }
        pthread_cond_wait(&g_mq_wake, &g_mq_lock);
    }
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

 klj_message *klj_as_message(void *obj) {
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
    m->arg1   = n > 1 ? (int32_t)a[1].j : 0;
    m->arg2   = n > 2 ? (int32_t)a[2].j : 0;
    m->target = self;                       // sendToTarget() needs to find it back
    void *obj = klj_new_object_data("android/os/Message", m);
    // Pinned because it is queued and outlives the frame that made it — and
    // taken OUT of that frame for the same reason. Both halves are needed: the
    // pin stops the frame pop retiring it early, and the frame no longer
    // listing it is what makes klj_deliver_message's retire the only one. A
    // frame entry that survives its object is how a pop retires whoever
    // recycled the slot, and the creating frame is on a different thread from
    // the delivery, so klj_frame_forget could never reach it from there.
    pthread_mutex_lock(&g_lock);
    klj_as_object(obj)->pinned  = 1;
    klj_as_object(obj)->destroy = free;   // obj/target are pool jobjects, not ours
    klj_frame_forget(klj_as_object(obj));
    pthread_mutex_unlock(&g_lock);

    // Message's members are public *fields*, not getters, so the handler reads
    // msg.what with GetIntField. Publishing them through the same per-object write
    // table the guest's own Set*Field uses means a read finds them by the ordinary
    // path — no instance-field special case, and a guest that writes one back gets
    // the write it expects.
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "what", "I", 'f'),
                    (klj_val){.j = (uint64_t)(int64_t)m->what});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg1", "I", 'f'),
                    (klj_val){.j = (uint64_t)(int64_t)m->arg1});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg2", "I", 'f'),
                    (klj_val){.j = (uint64_t)(int64_t)m->arg2});
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

 klj_val klj_HandlerThread_start(void *env, void *self, const klj_val *a, int n) {
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

// HandlerThread.quit() / quitSafely(), Thread.isAlive(), join() and interrupt()
// — the other end of a HandlerThread's life, which nothing here had needed while
// every such thread lived as long as the run. Unity's VideoPlayer is the first to
// take one down: a clip that has played to its end quits the thread its frame
// callbacks ran on and then asks whether it is still alive (AC Nexus, at the end
// of each loading-screen clip — and an unanswered isAlive() is an abort).
//
// The two quits are one here: "safely" means deliver what is already due first,
// and nothing is ever queued for the future on these loopers.
static klj_looper *klj_handlerthread_looper(void *self) {
    klj_object *o = klj_as_object(self);
    return (o && strcmp(o->cls, "android/os/HandlerThread") == 0) ? o->data : NULL;
}
static klj_val klj_HandlerThread_quit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_handlerthread_looper(self);
    if (!lp || !lp->started) return (klj_val){.j = 0};
    pthread_mutex_lock(&lp->lock);
    lp->running = 0;
    pthread_cond_broadcast(&lp->wake);
    pthread_mutex_unlock(&lp->lock);
    KLJ_LOG("HandlerThread.quit() — looper thread told to stop");
    return (klj_val){.j = 1};
}
// isAlive() is true between start() and the thread leaving its loop. Any other
// Thread object the guest holds came from Thread.currentThread(), which is by
// construction a thread that is running.
static klj_val klj_Thread_isAlive(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_looper *lp = klj_handlerthread_looper(self);
    if (!lp) return (klj_val){.j = o && strcmp(o->cls, "java/lang/Thread") == 0};
    pthread_mutex_lock(&lp->lock);
    int alive = lp->started && !lp->finished;
    pthread_mutex_unlock(&lp->lock);
    return (klj_val){.j = alive};
}
// join() / join(millis): wait for the looper thread to end, as Java does.
//
// It has to WAIT, and the first version did not when the thread had not been
// told to stop yet. Unity ends its video thread by sending it a message whose
// callback quits the looper from inside, and joins straight after: the callback
// object lives on the caller's stack, so a join that returns early lets the
// caller destroy it while the message is still queued — the looper then calls
// into a destroyed object ("Pure virtual function called!", AC Nexus at the end
// of a loading clip).
//
// The one case that cannot be waited out is a thread nobody ever stops; that
// would hang the caller for the rest of the run, so once the queue is drained
// and the thread has sat idle for a second it is said and given up. From the
// looper's own thread a join is a deadlock in Java too, and returns.
static klj_val klj_Thread_join(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_handlerthread_looper(self);
    if (!lp || !lp->started) return (klj_val){.j = 0};
    if (pthread_equal(lp->thread, pthread_self())) return (klj_val){.j = 0};
    int idle_ms = 0, reap = 0;
    for (;;) {
        pthread_mutex_lock(&lp->lock);
        int finished = lp->finished, quiet = !lp->count && !lp->busy && lp->running;
        if (finished && !lp->joined) { lp->joined = 1; reap = 1; }
        pthread_mutex_unlock(&lp->lock);
        if (finished) break;
        idle_ms = quiet ? idle_ms + 2 : 0;
        if (idle_ms >= 1000) {
            KLJ_LOG("Thread.join() on a HandlerThread that is idle and was never "
                    "quit — given up after a second rather than hanging the caller");
            return (klj_val){.j = 0};
        }
        usleep(2000);
    }
    if (reap) pthread_join(lp->thread, NULL);
    return (klj_val){.j = 0};
}
static klj_val klj_Thread_interrupt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_handlerthread_looper(self);
    if (lp && lp->started) {
        pthread_mutex_lock(&lp->lock);
        pthread_cond_broadcast(&lp->wake);
        pthread_mutex_unlock(&lp->lock);
    }
    return (klj_val){.j = 0};
}
// Message.getTarget() and Handler.getLooper(): how the callback that ends
// Unity's video thread finds the looper to quit — handleMessage is handed the
// Message, and from it walks to the Handler it was sent through and that
// Handler's Looper. On the looper's own thread the answer is that thread's
// Looper object, which is kept for its life; the one the Handler was built
// with was a local of whoever built it.
static klj_val klj_Message_getTarget(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_message *m = klj_as_message(self);
    return (klj_val){.l = m ? m->target : NULL};
}
static klj_val klj_Handler_getLooper(void *env, void *self, const klj_val *a, int n) {
    klj_handler *h = klj_as_handler(self);
    klj_looper *lp = h ? klj_looper_of(h->looper) : NULL;
    if (lp && lp == g_tls_looper) return (klj_val){.l = klj_looper_thread_object()};
    if (lp) return (klj_val){.l = h->looper};
    return klj_Looper_getMainLooper(env, self, a, n);
}

// The rest of what Unity's teardown names beside those (read off libunity's
// method-name table rather than waited for one abort at a time): each has an
// answer that is simply true of these loopers. Nothing is ever pending on one
// that a removal could cancel, no thread here is ever interrupted, and the id
// is Linux's gettid() as the guest would see it — the thread's own, which the
// caller only compares or logs.
static klj_val klj_looper_nothing(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_HandlerThread_getThreadId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_handlerthread_looper(self);
    if (!lp || !lp->started) return (klj_val){.j = -1};
    return (klj_val){.j = (int32_t)pthread_mach_thread_np(lp->thread)};
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
 void *g_frame_callback;

// Forward — kl_jni_tick_choreographer() is defined later, beside the message
// delivery machinery; the frame-clock thread needs this and no other hook.
void kl_jni_tick_choreographer(void);

// ---- the frame clock -----------------------------------------------
//
// On Android the Choreographer's doFrame fires CONTINUOUSLY, once per display
// refresh, from the system — wholly independent of the render thread. Unity
// 1.40 (via the Android Game SDK's Swappy) waits on a refresh COUNTER that
// each doFrame advances by one, and the pump calling nativeRender directly
// blocked forever because it could deliver a doFrame only BEFORE the call and
// could not deliver more while inside it. The fixes are exactly as device
// reality is structured: the frame clock is a free-running source of its own,
// not a side effect of the render pump. Its cadence is read from the same
// display-frequency seam the compositor pushes (kl_ovrp_display_frequency),
// so it moves with the stated device — 72 Hz Quest-2 fiction on the host, the
// real drawable rate once a visionOS frontend pushes it.
//
// The thread calls guest code (JNIBridge.invoke -> doFrame -> the engine's own
// nOnChoreographer) while the main thread may be inside nativeRender. That is
// exactly Android: the vsync thread and the render thread are different
// threads, and the engine's own bookkeeping around these counters is locked
// (the wait side and the counter read hold the same mutex).
static int         g_frame_clock_running;
static void *klj_frame_clock_main(void *arg) {
    (void)arg;
    kl_jni_env();                     // per-thread env + kl_thread_init
    while (g_frame_clock_running) {
        double hz = kl_ovrp_display_frequency();
        if (!(hz >= 30.0 && hz <= 240.0)) hz = 72.0;
        struct timespec d = { 0, (long)(1e9 / hz) };
        nanosleep(&d, NULL);
        if (g_frame_callback)
            kl_jni_tick_choreographer();
    }
    return NULL;
}
static void klj_frame_clock_start(void) {
    static int started;
    if (started) return;
    started = 1;
    g_frame_clock_running = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, klj_frame_clock_main, NULL) == 0)
        pthread_detach(th);
}

static klj_val klj_Choreographer_postFrameCallback(void *env, void *self,
                                                   const klj_val *a, int n) {
    (void)env; (void)self;
    g_frame_callback = n > 0 ? a[0].l : NULL;
    // The moment a callback exists the frame clock must be LIVE on its own
    // thread — the render pump may block inside nativeRender waiting on the
    // refresh counter at any moment, and only an independent source can keep
    // advancing it then. See the frame-clock block above.
    klj_frame_clock_start();
    if (g_frame_callback) {
        static int announced;
        if (!announced) {
            announced = 1;
            klj_object *o = klj_as_object(g_frame_callback);
            KLJ_LOG("Choreographer.postFrameCallback(%s) — the frame clock now runs "
                    "on its own host thread at the seam's display frequency",
                    o ? o->cls : "(untagged)");
        }
    }
    return (klj_val){.j = 0};
}

// kl_jni_tick_choreographer() lives further down, next to the message delivery —
// both call into guest proxies, and that machinery is defined there.

// ---- android.os.Process ----
// setThreadPriority(tid, priority) is an Android nice value applied to another
// thread by tid. Darwin has no by-nice equivalent — scheduling is set through
// pthread QoS classes — but the ONE case that matters we CAN honour: a thread
// flagged THREAD_PRIORITY_AUDIO (-16) or _URGENT_AUDIO (-19) is an audio mixer
// thread with a realtime deadline, and on visionOS a default-QoS guest thread is
// scheduled BEHIND the compositor. Under shader-compile / render load that mixer
// starves and its output arrives in gaps — the stutter olar exhibits even though
// our CoreAudio output path never underruns (the gaps are in the content the
// guest produced). So for an audio priority we route the tid through the same
// QoS-override path XR-critical threads use (kl_pthread_boost_qos: self door for
// the calling thread, cross-thread override via the live registry otherwise).
// The tid the guest passes is our pthread_threadid_np — gettid / Process.myTid
// both return it — so the registry lookup matches. Below-normal / non-audio
// priorities stay recorded-not-applied: boosting the game or render thread here
// would starve the very compositor this is careful not to.
#define KLJ_THREAD_PRIORITY_AUDIO (-16)   // android.os.Process.THREAD_PRIORITY_AUDIO
static klj_val klj_Process_setThreadPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n > 1) {
        int tid = (int)a[0].j, prio = (int)a[1].j;
        if (prio <= KLJ_THREAD_PRIORITY_AUDIO) {
            KLJ_LOG("Process.setThreadPriority(tid=%d, %d) — audio thread, "
                    "raising to USER_INTERACTIVE QoS", tid, prio);
            kl_pthread_boost_qos((uint32_t)tid);
        } else {
            KLJ_LOG("Process.setThreadPriority(tid=%d, %d) — not applied on Darwin",
                    tid, prio);
        }
    } else if (n > 0) {
        int prio = (int)a[0].j;
        if (prio <= KLJ_THREAD_PRIORITY_AUDIO) {
            uint64_t self_tid = 0; pthread_threadid_np(NULL, &self_tid);
            KLJ_LOG("Process.setThreadPriority(%d) — audio, raising self to "
                    "USER_INTERACTIVE QoS", prio);
            kl_pthread_boost_qos(self_tid);
        } else {
            KLJ_LOG("Process.setThreadPriority(%d) — not applied on Darwin", prio);
        }
    }
    return (klj_val){0};
}
// SDL's own wrapper for the same operation, and it lands in the same place:
// it is Process.setThreadPriority(THREAD_PRIORITY_AUDIO or _URGENT_AUDIO) on
// the CALLING thread, which Darwin has no by-tid equivalent for. Recorded, not
// applied — see klj_Process_setThreadPriority above for why.
//
// Worth noting what this does NOT cost us: SDL raises the priority of a thread
// that feeds the audio device, and our audio path does not use it. kl_audio.c's
// CoreAudio render callback runs on the OS's own realtime thread and the device
// provides the clock, so the guest's feeder is a producer into a ring rather
// than something with a deadline.
static klj_val klj_SDLAM_audioSetThreadPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Called ON the SDL audio thread to raise its own priority. The Java
    // Process.setThreadPriority(AUDIO) path already maps that to QoS; do the
    // same for this thread, or its buffer refills are scheduled behind the
    // compositor and underrun — the periodic half-second audio dropouts Steam
    // Link shows. Self, so the boost is exact (no tid to map).
    uint64_t self_tid = 0; pthread_threadid_np(NULL, &self_tid);
    kl_pthread_boost_qos(self_tid);
    return (klj_val){0};
}

// SDLAudioManager.unregisterAudioDeviceCallback() — drop the AudioDeviceCallback
// SDL registered so it can hear devices appear and disappear.
//
// A no-op, and the smali is what says so: the body does nothing but hand the
// callback back to AudioManager.unregisterAudioDeviceCallback and return. There
// is nothing to drop — `AudioManager.getDevices()` is answered EMPTY here (host
// devices are deliberately not enumerated), so the set SDL was watching never
// had a member and never changed. Same reasoning, and the same shape, as
// DisplayManager.registerDisplayListener: the thing being subscribed to cannot
// vary, so nothing is owed a callback and nothing is lost by forgetting one.
//
// Reached on TEARDOWN, which is why it took until a run got far enough to shut
// down cleanly to reach it: the app puts up its own "no streaming host" message
// box and then exits.
static klj_val klj_SDLAM_unregisterAudioDeviceCallback(void *env, void *self,
                                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("SDLAudioManager.unregisterAudioDeviceCallback() — no devices were "
            "enumerated, so nothing was subscribed");
    return (klj_val){0};
}

static klj_val klj_Process_myTid(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (klj_val){.j = (uint32_t)tid};
}

// Unity's GLES VideoPlayer consumes MediaCodec frames via SurfaceTexture.
typedef struct { void *reader, *listener, *object; pthread_mutex_t lock; } klj_surface_texture;
static klj_surface_texture *surface_texture(void *self) {
    klj_object *o = klj_as_object(self);
    return o && !strcmp(o->cls, "android/graphics/SurfaceTexture") ? o->data : NULL;
}
static void surface_frame(void *ctx, void *reader) {
    (void)reader;
    klj_surface_texture *st = ctx;
    pthread_mutex_lock(&st->lock);
    void *listener = st->listener;
    pthread_mutex_unlock(&st->lock);
    if (!listener) return;
    void *env = kl_jni_env();
    klj_PushLocalFrame(env, 8);
    void *args = klj_new_array('L', "java/lang/Object", 1);
    ((void **)klj_arr(args)->data)[0] = st->object;
    klj_proxy_invoke(listener, "android/graphics/SurfaceTexture$OnFrameAvailableListener",
                     "onFrameAvailable", "(Landroid/graphics/SurfaceTexture;)V", args);
    klj_PopLocalFrame(env, NULL);
}
static klj_val surface_texture_init(void *env, void *cls, const klj_val *a, int n) {
    (void)env; (void)cls;
    klj_surface_texture *st = calloc(1, sizeof *st);
    if (!st) return (klj_val){0};
    st->reader = kl_mediandk_texture_new(n ? (unsigned)a[0].j : 0);
    if (!st->reader) { free(st); return (klj_val){0}; }
    pthread_mutex_init(&st->lock, NULL);
    st->object = klj_new_object_data("android/graphics/SurfaceTexture", st);
    kl_jni_pin_object(st->object); // retained by producer callbacks until release
    return (klj_val){.l=st->object};
}
static klj_val surface_init(void *env, void *cls, const klj_val *a, int n) {
    (void)env; (void)cls;
    void *texture = n ? a[0].l : NULL;
    return (klj_val){.l=klj_new_object_data("android/view/Surface", texture)};
}
void *kl_jni_surface_window(void *surface) {
    klj_object *o = klj_as_object(surface);
    klj_surface_texture *st = o && !strcmp(o->cls, "android/view/Surface") ? surface_texture(o->data) : NULL;
    return st && st->reader ? kl_mediandk_texture_window(st->reader) : NULL;
}
static klj_val surface_listener(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_surface_texture *st = surface_texture(self);
    if (!st || !st->reader) return (klj_val){0};
    void *listener = n ? a[0].l : NULL;
    if (listener) kl_jni_pin_object(listener);
    pthread_mutex_lock(&st->lock);
    st->listener = listener;
    pthread_mutex_unlock(&st->lock);
    kl_mediandk_texture_listener(st->reader, st, listener ? surface_frame : NULL);
    return (klj_val){0};
}
static klj_val surface_update(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_surface_texture *st=surface_texture(self);
    if (st && st->reader) kl_mediandk_texture_update(st->reader);
    return (klj_val){0};
}
static klj_val surface_timestamp(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_surface_texture *st=surface_texture(self);
    return (klj_val){.j=st && st->reader ? kl_mediandk_texture_timestamp(st->reader) : 0};
}
static klj_val surface_matrix(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_array *ar=n ? klj_arr(a[0].l) : NULL;
    if (!ar || ar->kind!='F' || ar->len<16) { klj_raise("java/lang/IllegalArgumentException", "matrix needs 16 floats"); return (klj_val){0}; }
    float *m=ar->data; memset(m,0,16*sizeof *m);
    m[0]=m[10]=m[15]=1; m[5]=-1; m[13]=1;
    return (klj_val){0};
}
static klj_val surface_size(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_surface_texture *st=surface_texture(self);
    if (st && st->reader && n==2) kl_mediandk_texture_size(st->reader,a[0].j,a[1].j);
    return (klj_val){0};
}
static klj_val surface_attach(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_surface_texture *st=surface_texture(self);
    if (st && st->reader && n) kl_mediandk_texture_attach(st->reader,a[0].j);
    return (klj_val){0};
}
static klj_val surface_detach(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_surface_texture *st=surface_texture(self);
    if (st && st->reader) kl_mediandk_texture_detach(st->reader);
    return (klj_val){0};
}
static klj_val surface_released(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_surface_texture *st=surface_texture(self);
    return (klj_val){.j=!st || !st->reader};
}
static klj_val surface_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_surface_texture *st=surface_texture(self);
    if (st && st->reader) { kl_mediandk_texture_delete(st->reader); st->reader=NULL; st->listener=NULL; }
    return (klj_val){0};
}
static klj_val surface_wrapper_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o=klj_as_object(self); if (o) o->data=NULL;
    return (klj_val){0};
}

const klj_binding klj_bind_looper[] = {
    {"android/graphics/SurfaceTexture", "<init>", "(I)V", surface_texture_init},
    {"android/view/Surface", "<init>", "(Landroid/graphics/SurfaceTexture;)V", surface_init},
    {"android/view/Surface", "release", "()V", surface_wrapper_release},
    {"android/graphics/SurfaceTexture", "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V", surface_listener},
    {"android/graphics/SurfaceTexture", "updateTexImage", "()V", surface_update},
    {"android/graphics/SurfaceTexture", "getTimestamp", "()J", surface_timestamp},
    {"android/graphics/SurfaceTexture", "getTransformMatrix", "([F)V", surface_matrix},
    {"android/graphics/SurfaceTexture", "setDefaultBufferSize", "(II)V", surface_size},
    {"android/graphics/SurfaceTexture", "attachToGLContext", "(I)V", surface_attach},
    {"android/graphics/SurfaceTexture", "detachFromGLContext", "()V", surface_detach},
    {"android/graphics/SurfaceTexture", "isReleased", "()Z", surface_released},
    {"android/graphics/SurfaceTexture", "release", "()V", surface_release},

    {"android/app/Activity",   "runOnUiThread", "(Ljava/lang/Runnable;)V", klj_Activity_runOnUiThread},

    {"org/libsdl/app/SDLAudioManager", "unregisterAudioDeviceCallback", "()V",
     klj_SDLAM_unregisterAudioDeviceCallback},
    {"org/libsdl/app/SDLAudioManager", "audioSetThreadPriority", "(ZI)V",
     klj_SDLAM_audioSetThreadPriority},
    {"android/os/Process", "setThreadPriority", "(II)V", klj_Process_setThreadPriority},
    {"android/os/Process", "setThreadPriority", "(I)V",  klj_Process_setThreadPriority},
    {"android/os/Process", "myTid",             "()I",   klj_Process_myTid},

    {"android/os/Looper",  "getMainLooper", "()Landroid/os/Looper;",  klj_Looper_getMainLooper},
    {"android/os/Looper",  "myLooper",      "()Landroid/os/Looper;",  klj_Looper_myLooper},
    {"android/os/Looper",  "prepare",       "()V",                    klj_Looper_prepare},
    {"android/os/Looper",  "getQueue",      "()Landroid/os/MessageQueue;", klj_Looper_getQueue},
    {"android/os/Looper",  "quit",          "()V",                    klj_Looper_quit},
    {"android/os/Looper",  "quitSafely",    "()V",                    klj_Looper_quit},
    {"android/os/MessageQueue", "next",     "()Landroid/os/Message;", klj_MessageQueue_next},
    // A HandlerThread is a thread with a Looper on it. We have one queue and one
    // drain point (kl_jni_drain_ui_tasks) for the main looper — but a
    // HandlerThread gets a real thread of its own, because the guest blocks
    // waiting on it. See the looper section above.
    {"android/os/Handler", "obtainMessage", "(I)Landroid/os/Message;", klj_Handler_obtainMessage},
    // Unity video preparation posts a message with two integer arguments.
    {"android/os/Handler", "obtainMessage", "(III)Landroid/os/Message;", klj_Handler_obtainMessage},
    {"android/os/Message", "sendToTarget", "()V", klj_Message_sendToTarget},
    {"android/os/Message", "getTarget", "()Landroid/os/Handler;", klj_Message_getTarget},
    {"android/os/Handler", "getLooper", "()Landroid/os/Looper;", klj_Handler_getLooper},
    {"android/view/Choreographer", "getInstance", "()Landroid/view/Choreographer;", klj_Choreographer_getInstance},
    {"android/view/Choreographer", "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", klj_Choreographer_postFrameCallback},
    {"android/os/HandlerThread", "<init>", "(Ljava/lang/String;)V", klj_HandlerThread_init},
    {"android/os/HandlerThread", "start", "()V", klj_HandlerThread_start},
    {"android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", klj_HandlerThread_getLooper},
    {"android/os/HandlerThread", "quit",       "()Z", klj_HandlerThread_quit},
    {"android/os/HandlerThread", "quitSafely", "()Z", klj_HandlerThread_quit},
    // Asked of java/lang/Thread (the declaring class) and of the HandlerThread
    // itself, depending on how the caller resolved the method.
    {"java/lang/Thread",         "isAlive",   "()Z",  klj_Thread_isAlive},
    {"android/os/HandlerThread", "isAlive",   "()Z",  klj_Thread_isAlive},
    {"java/lang/Thread",         "join",      "()V",  klj_Thread_join},
    {"java/lang/Thread",         "join",      "(J)V", klj_Thread_join},
    {"android/os/HandlerThread", "join",      "()V",  klj_Thread_join},
    {"android/os/HandlerThread", "join",      "(J)V", klj_Thread_join},
    {"java/lang/Thread",         "interrupt", "()V",  klj_Thread_interrupt},
    {"java/lang/Thread",         "isInterrupted", "()Z", klj_looper_nothing},
    {"android/os/HandlerThread", "isInterrupted", "()Z", klj_looper_nothing},
    {"android/os/HandlerThread", "getThreadId", "()I", klj_HandlerThread_getThreadId},
    {"android/os/Handler", "removeCallbacksAndMessages", "(Ljava/lang/Object;)V", klj_looper_nothing},
    {"android/os/HandlerThread", "interrupt", "()V",  klj_Thread_interrupt},
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
    {0}
};
