// Android NDK shim. See kl_ndk.h for the measured scope.
//
// Three independent pieces:
//   ALooper       — real implementation over poll() and a self-pipe. Unity waits
//                   on this, so a stub that returns immediately would spin.
//   ANativeWindow — a synthetic window carrying geometry. No pixels until M5.
//   ASensor       — an empty sensor list, which is a legitimate Android device
//                   configuration and the honest answer here (see below).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kl_ndk.h"

// ================================================================== ALooper
enum {
    ALOOPER_POLL_WAKE     = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT  = -3,
    ALOOPER_POLL_ERROR    = -4,
};
enum { ALOOPER_EVENT_INPUT = 1 << 0 };

#define KL_LOOPER_MAX_FDS 16

typedef int (*kl_looper_cb)(int fd, int events, void *data);

typedef struct kl_looper {
    int             refs;
    int             wake_r, wake_w;
    struct {
        int          fd, ident, events;
        kl_looper_cb cb;
        void        *data;
    } fds[KL_LOOPER_MAX_FDS];
    int             nfds;
    pthread_mutex_t lock;
} kl_looper;

static pthread_key_t  g_looper_key;
static pthread_once_t g_looper_once = PTHREAD_ONCE_INIT;
static void kl_looper_key_init(void) { pthread_key_create(&g_looper_key, NULL); }

static kl_looper *kl_ALooper_forThread(void) {
    pthread_once(&g_looper_once, kl_looper_key_init);
    return pthread_getspecific(g_looper_key);
}

static kl_looper *kl_ALooper_prepare(int opts) {
    kl_looper *l = kl_ALooper_forThread();
    if (l) return l;

    l = calloc(1, sizeof *l);
    if (!l) return NULL;
    int p[2];
    if (pipe(p) != 0) { free(l); return NULL; }
    // Non-blocking so draining the wake pipe never stalls the loop.
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);
    l->wake_r = p[0];
    l->wake_w = p[1];
    l->refs   = 1;               // the reference held by the thread itself
    (void)opts;
    pthread_mutex_init(&l->lock, NULL);
    pthread_setspecific(g_looper_key, l);
    return l;
}

static void kl_ALooper_acquire(kl_looper *l) {
    if (l) __atomic_fetch_add(&l->refs, 1, __ATOMIC_RELAXED);
}

static void kl_ALooper_release(kl_looper *l) {
    // Deliberately never frees. A looper is thread-owned and long-lived, and a
    // use-after-free here would surface as an unrelated poll() failure much
    // later. One leaked looper per thread is the cheaper trade.
    if (l) __atomic_fetch_sub(&l->refs, 1, __ATOMIC_RELAXED);
}

static void kl_ALooper_wake(kl_looper *l) {
    if (!l) return;
    char c = 1;
    ssize_t n = write(l->wake_w, &c, 1);
    (void)n;   // EAGAIN just means a wake is already pending, which is the point
}

static int kl_ALooper_addFd(kl_looper *l, int fd, int ident, int events,
                            kl_looper_cb cb, void *data) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {          // re-registering replaces
            l->fds[i].ident = ident; l->fds[i].events = events;
            l->fds[i].cb = cb; l->fds[i].data = data;
            pthread_mutex_unlock(&l->lock);
            return 1;
        }
    if (l->nfds == KL_LOOPER_MAX_FDS) { pthread_mutex_unlock(&l->lock); return -1; }
    l->fds[l->nfds++] = (typeof(l->fds[0])){fd, ident, events, cb, data};
    pthread_mutex_unlock(&l->lock);
    return 1;
}

static int kl_ALooper_removeFd(kl_looper *l, int fd) {
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->nfds; i++)
        if (l->fds[i].fd == fd) {
            l->fds[i] = l->fds[--l->nfds];
            pthread_mutex_unlock(&l->lock);
            return 1;
        }
    pthread_mutex_unlock(&l->lock);
    return 0;
}

static int kl_looper_poll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    kl_looper *l = kl_ALooper_forThread();
    if (!l) return ALOOPER_POLL_ERROR;

    struct pollfd pfd[KL_LOOPER_MAX_FDS + 1];
    int           ident[KL_LOOPER_MAX_FDS + 1];
    kl_looper_cb  cb[KL_LOOPER_MAX_FDS + 1];
    void         *data[KL_LOOPER_MAX_FDS + 1];

    pthread_mutex_lock(&l->lock);
    pfd[0] = (struct pollfd){l->wake_r, POLLIN, 0};
    int n = 1;
    for (int i = 0; i < l->nfds; i++, n++) {
        pfd[n]   = (struct pollfd){l->fds[i].fd,
                                   (short)(l->fds[i].events & ALOOPER_EVENT_INPUT ? POLLIN : 0), 0};
        ident[n] = l->fds[i].ident;
        cb[n]    = l->fds[i].cb;
        data[n]  = l->fds[i].data;
    }
    pthread_mutex_unlock(&l->lock);

    int r = poll(pfd, (nfds_t)n, timeoutMillis);
    if (r < 0)  return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;

    if (pfd[0].revents & POLLIN) {         // drain the wake pipe
        char buf[64];
        while (read(l->wake_r, buf, sizeof buf) > 0) { }
        return ALOOPER_POLL_WAKE;
    }
    for (int i = 1; i < n; i++) {
        if (!pfd[i].revents) continue;
        if (cb[i]) { cb[i](pfd[i].fd, pfd[i].revents, data[i]); return ALOOPER_POLL_CALLBACK; }
        if (outFd)     *outFd     = pfd[i].fd;
        if (outEvents) *outEvents = pfd[i].revents;
        if (outData)   *outData   = data[i];
        return ident[i];
    }
    return ALOOPER_POLL_WAKE;
}

static int64_t kl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// pollAll differs from pollOnce only in that it does not return
// ALOOPER_POLL_CALLBACK to the caller — it keeps going until something the
// caller cares about happens.
//
// The timeout is a deadline, not a per-iteration budget: re-polling with the
// full timeout after each callback would let this overshoot without bound.
// Nothing triggers a callback today (the sensor queue never produces events),
// but frame pacing is this project's hardest open risk (§6 M6) and a looper
// that sleeps longer than asked is exactly the kind of thing that shows up
// later as judder and is miserable to trace back.
static int kl_ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    int64_t deadline = timeoutMillis > 0 ? kl_now_ms() + timeoutMillis : 0;
    int     remaining = timeoutMillis;
    for (;;) {
        int r = kl_looper_poll(remaining, outFd, outEvents, outData);
        if (r != ALOOPER_POLL_CALLBACK) return r;
        if (timeoutMillis == 0) return ALOOPER_POLL_CALLBACK;
        if (timeoutMillis > 0) {
            int64_t left = deadline - kl_now_ms();
            if (left <= 0) return ALOOPER_POLL_TIMEOUT;
            remaining = (int)left;
        }
        // timeoutMillis < 0 means block indefinitely; remaining stays negative.
    }
}
static int kl_ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    return kl_looper_poll(timeoutMillis, outFd, outEvents, outData);
}

// ============================================================ ANativeWindow
typedef struct kl_native_window {
    int     refs;
    int32_t width, height, format;
} kl_native_window;

// Quest 2 per-eye geometry, which is what this title was built against. Only a
// placeholder: in VR the eye buffers come from the XR runtime (M6), and this
// surface exists mainly to give Unity a non-zero Screen size at startup. The
// host overrides it via kl_ndk_set_window once a real drawable exists (M5).
static kl_native_window g_window = {1, 1832, 1920, 1 /* RGBA_8888 */};

void kl_ndk_set_window(int32_t w, int32_t h, int32_t format) {
    g_window.width = w; g_window.height = h; g_window.format = format;
}
void *kl_ndk_window(void) { return &g_window; }

static void    kl_ANativeWindow_acquire(kl_native_window *w) { if (w) __atomic_fetch_add(&w->refs, 1, __ATOMIC_RELAXED); }
static void    kl_ANativeWindow_release(kl_native_window *w) { if (w) __atomic_fetch_sub(&w->refs, 1, __ATOMIC_RELAXED); }
static int32_t kl_ANativeWindow_getWidth(kl_native_window *w)  { return w ? w->width  : 0; }
static int32_t kl_ANativeWindow_getHeight(kl_native_window *w) { return w ? w->height : 0; }
static int32_t kl_ANativeWindow_getFormat(kl_native_window *w) { return w ? w->format : 0; }

static void *kl_ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env; (void)surface;
    kl_ANativeWindow_acquire(&g_window);
    return &g_window;
}

// Android treats 0 for any parameter as "keep the default". Honour that:
// Unity calls this with 0,0 to reset geometry after a format-only change, and
// clamping to zero there would make Screen.width report 0.
static int32_t kl_ANativeWindow_setBuffersGeometry(kl_native_window *w,
                                                   int32_t width, int32_t height, int32_t format) {
    if (!w) return -EINVAL;
    if (width > 0)  w->width  = width;
    if (height > 0) w->height = height;
    if (format > 0) w->format = format;
    return 0;
}

// =================================================================== ASensor
// The sensor list is empty, and that is the correct answer rather than a
// shortcut. Vision Pro exposes no Android-shaped accelerometer/gyro to the
// guest, and this title does not want one: head and controller poses arrive
// through ovrp_* (M6), not through Input.acceleration. An empty list is a
// configuration real Android devices ship, so Unity already handles it —
// whereas a fabricated sensor would feed the engine invented motion data.
//
// The accessors are still real, so adding an entry later is a one-line change.
typedef struct kl_sensor {
    const char *name, *vendor;
    int         type, min_delay_us;
    float       resolution;
} kl_sensor;

static const kl_sensor  *g_sensors[]  = { NULL };   // C forbids a zero-length array
static const unsigned    g_nsensors   = 0;

typedef struct kl_sensor_queue {
    kl_looper   *looper;
    int          ident;
    kl_looper_cb cb;
    void        *data;
    int          pipe_r, pipe_w;   // never written: we have no sensors to report
} kl_sensor_queue;

static char g_sensor_manager;      // opaque, non-NULL handle

static void *kl_ASensorManager_getInstance(void) { return &g_sensor_manager; }
static void *kl_ASensorManager_getInstanceForPackage(const char *pkg) { (void)pkg; return &g_sensor_manager; }

static int kl_ASensorManager_getSensorList(void *mgr, const kl_sensor ***list) {
    (void)mgr;
    if (list) *list = g_sensors;
    return (int)g_nsensors;
}

static const kl_sensor *kl_ASensorManager_getDefaultSensor(void *mgr, int type) {
    (void)mgr;
    for (unsigned i = 0; i < g_nsensors; i++)
        if (g_sensors[i]->type == type) return g_sensors[i];
    return NULL;
}

static void *kl_ASensorManager_createEventQueue(void *mgr, kl_looper *looper, int ident,
                                                kl_looper_cb cb, void *data) {
    (void)mgr;
    kl_sensor_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    int p[2];
    if (pipe(p) != 0) { free(q); return NULL; }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    q->looper = looper; q->ident = ident; q->cb = cb; q->data = data;
    q->pipe_r = p[0]; q->pipe_w = p[1];
    // Register with the looper even though nothing will ever be readable, so
    // the caller's poll ident bookkeeping matches what it would see on Android.
    if (looper) kl_ALooper_addFd(looper, q->pipe_r, ident, ALOOPER_EVENT_INPUT, cb, data);
    return q;
}

static int kl_ASensorManager_destroyEventQueue(void *mgr, kl_sensor_queue *q) {
    (void)mgr;
    if (!q) return -EINVAL;
    if (q->looper) kl_ALooper_removeFd(q->looper, q->pipe_r);
    close(q->pipe_r); close(q->pipe_w);
    free(q);
    return 0;
}

// Unreachable while the sensor list is empty — the guest cannot obtain a valid
// ASensor* to pass. They return the same -EINVAL bionic would.
static int kl_ASensorEventQueue_enableSensor(void *q, const void *s)  { (void)q; (void)s; return -EINVAL; }
static int kl_ASensorEventQueue_disableSensor(void *q, const void *s) { (void)q; (void)s; return -EINVAL; }
static int kl_ASensorEventQueue_setEventRate(void *q, const void *s, int32_t us) { (void)q; (void)s; (void)us; return -EINVAL; }
static int kl_ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }

// ssize_t: 0 means "no events pending", which is not an error.
static ssize_t kl_ASensorEventQueue_getEvents(void *q, void *events, size_t count) {
    (void)q; (void)events; (void)count;
    return 0;
}

static const char *kl_ASensor_getName(const kl_sensor *s)   { return s ? s->name   : ""; }
static const char *kl_ASensor_getVendor(const kl_sensor *s) { return s ? s->vendor : ""; }
static int   kl_ASensor_getType(const kl_sensor *s)         { return s ? s->type : -1 /* ASENSOR_TYPE_INVALID */; }
static float kl_ASensor_getResolution(const kl_sensor *s)   { return s ? s->resolution : 0.0f; }
static int   kl_ASensor_getMinDelay(const kl_sensor *s)     { return s ? s->min_delay_us : 0; }

// ================================================================== dispatch
static const struct { const char *name; void *fn; } g_ndk[] = {
#define N(sym, fn) {sym, (void *)(fn)}
    N("ALooper_forThread",  kl_ALooper_forThread),
    N("ALooper_prepare",    kl_ALooper_prepare),
    N("ALooper_acquire",    kl_ALooper_acquire),
    N("ALooper_release",    kl_ALooper_release),
    N("ALooper_pollOnce",   kl_ALooper_pollOnce),
    N("ALooper_pollAll",    kl_ALooper_pollAll),
    N("ALooper_wake",       kl_ALooper_wake),
    N("ALooper_addFd",      kl_ALooper_addFd),
    N("ALooper_removeFd",   kl_ALooper_removeFd),

    N("ANativeWindow_acquire",            kl_ANativeWindow_acquire),
    N("ANativeWindow_release",            kl_ANativeWindow_release),
    N("ANativeWindow_fromSurface",        kl_ANativeWindow_fromSurface),
    N("ANativeWindow_getWidth",           kl_ANativeWindow_getWidth),
    N("ANativeWindow_getHeight",          kl_ANativeWindow_getHeight),
    N("ANativeWindow_getFormat",          kl_ANativeWindow_getFormat),
    N("ANativeWindow_setBuffersGeometry", kl_ANativeWindow_setBuffersGeometry),

    N("ASensorManager_getInstance",           kl_ASensorManager_getInstance),
    N("ASensorManager_getInstanceForPackage", kl_ASensorManager_getInstanceForPackage),
    N("ASensorManager_getSensorList",         kl_ASensorManager_getSensorList),
    N("ASensorManager_getDefaultSensor",      kl_ASensorManager_getDefaultSensor),
    N("ASensorManager_createEventQueue",      kl_ASensorManager_createEventQueue),
    N("ASensorManager_destroyEventQueue",     kl_ASensorManager_destroyEventQueue),
    N("ASensorEventQueue_enableSensor",       kl_ASensorEventQueue_enableSensor),
    N("ASensorEventQueue_disableSensor",      kl_ASensorEventQueue_disableSensor),
    N("ASensorEventQueue_setEventRate",       kl_ASensorEventQueue_setEventRate),
    N("ASensorEventQueue_hasEvents",          kl_ASensorEventQueue_hasEvents),
    N("ASensorEventQueue_getEvents",          kl_ASensorEventQueue_getEvents),
    N("ASensor_getName",                      kl_ASensor_getName),
    N("ASensor_getVendor",                    kl_ASensor_getVendor),
    N("ASensor_getType",                      kl_ASensor_getType),
    N("ASensor_getResolution",                kl_ASensor_getResolution),
    N("ASensor_getMinDelay",                  kl_ASensor_getMinDelay),
#undef N
};

void *kl_ndk_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_ndk / sizeof g_ndk[0]; i++)
        if (strcmp(g_ndk[i].name, name) == 0) return g_ndk[i].fn;
    return NULL;
}
