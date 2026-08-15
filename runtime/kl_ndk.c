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
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kl_ndk.h"
// For kl_jni_locale_parts — AConfiguration and java.util.Locale are two doors
// onto one fact and must not answer differently.
#include "kl_jni.h"

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

// Android's main thread ALWAYS has a looper: ActivityThread calls
// Looper.prepareMainLooper() long before any activity's onCreate, so
// ALooper_forThread() on that thread never returns NULL and native code is
// entitled to assume it. We author that side, so a host driving a
// NativeActivity has to have done the same thing first.
//
// libvrlink_scene is what forced this and it fails a long way from the cause:
// UIThreadCallbackHandler's constructor pipe()s, takes ALooper_forThread(),
// and calls ALooper_addFd() on it — and on -1 it throws std::bad_alloc, which
// is the *wrong* exception for "there was no looper" and sends the search
// straight to memory. The abort is 8 frames of libc++ terminate machinery with
// no mention of a looper anywhere.
void kl_ndk_prepare_looper(void) { (void)kl_ALooper_prepare(0); }

int kl_ndk_thread_has_looper(void) { return kl_ALooper_forThread() != NULL; }

static int kl_looper_poll(int timeoutMillis, int *outFd, int *outEvents, void **outData);

// ...and running it, which is the other half and is just as much ours to do.
// Android's main thread does not merely HAVE a looper, it is sitting in
// Looper.loop() whenever it is not inside a callback — that loop is what makes
// a posted message run. A host that prepares a looper and then sleeps has built
// the queue and left nothing draining it, which is the Choreographer lesson from
// M4 in a different subsystem: the guest is not stuck, it is waiting for a pump
// we declined to run.
int kl_ndk_pump_looper(int timeout_ms) {
    return kl_looper_poll(timeout_ms, NULL, NULL, NULL);
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
    // Non-NULL for a window somebody else made and owns — today that is an
    // AImageReader's output surface (kl_mediandk.c). Opaque here deliberately;
    // see kl_ndk.h.
    void   *owner;
} kl_native_window;

// Quest 2 per-eye geometry, which is what this title was built against. Only a
// placeholder: in VR the eye buffers come from the XR runtime (M6), and this
// surface exists mainly to give Unity a non-zero Screen size at startup. The
// host overrides it via kl_ndk_set_window once a real drawable exists (M5).
static kl_native_window g_window = {1, 1832, 1920, 1 /* RGBA_8888 */, NULL};

void kl_ndk_set_window(int32_t w, int32_t h, int32_t format) {
    g_window.width = w; g_window.height = h; g_window.format = format;
}
void *kl_ndk_window(void) { return &g_window; }

void kl_ndk_window_size(const void *win, int32_t *w, int32_t *h) {
    const kl_native_window *nw = win ? (const kl_native_window *)win : &g_window;
    if (w) *w = nw->width;
    if (h) *h = nw->height;
}

// Windows that are not the activity's. See kl_ndk.h for why these exist.
void *kl_ndk_window_new(int32_t w, int32_t h, int32_t format, void *owner) {
    kl_native_window *nw = calloc(1, sizeof *nw);
    if (!nw) return NULL;
    nw->refs = 1;
    nw->width = w; nw->height = h; nw->format = format;
    nw->owner = owner;
    return nw;
}

void kl_ndk_window_free(void *win) {
    // g_window is static storage and is the activity's for the life of the
    // process; freeing it would be a wild free, and a guest that releases a
    // surface it did not create is not doing anything unusual.
    if (win && win != (void *)&g_window) free(win);
}

void *kl_ndk_window_owner(const void *win) {
    return win ? ((const kl_native_window *)win)->owner : NULL;
}

void kl_ndk_window_set_size(void *win, int32_t w, int32_t h) {
    kl_native_window *nw = win ? (kl_native_window *)win : &g_window;
    nw->width = w; nw->height = h;
}

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

// The software path: SDL3's renderer is GLES2, but the lock/unlockAndPost pair
// is what a canvas renderer would paint through, and libmain imports both.
// The bits buffer is real memory so the guest can draw without faulting;
// nothing displays it — display goes through EGL (M5).
typedef struct { int32_t width, height, stride, format; void *bits; uint32_t rsvd[6]; }
    kl_window_buffer;
static void *g_lock_bits;
static size_t g_lock_bytes;

static int32_t kl_ANativeWindow_lock(kl_native_window *w, kl_window_buffer *buf, void *dirty) {
    (void)dirty;
    kl_native_window *nw = w ? w : &g_window;
    size_t need = (size_t)nw->width * (size_t)nw->height * 4;
    if (need > g_lock_bytes) {
        free(g_lock_bits);
        g_lock_bits = calloc(1, need);
        if (!g_lock_bits) return -ENOMEM;
        g_lock_bytes = need;
    }
    buf->width  = nw->width;
    buf->height = nw->height;
    buf->stride = nw->width;
    buf->format = nw->format;
    buf->bits   = g_lock_bits;
    return 0;
}
static int32_t kl_ANativeWindow_unlockAndPost(kl_native_window *w) { (void)w; return 0; }

// ==================================================================== AAsset
// Steam Link reads its assets through the NDK C API where Unity reads them
// over JNI (see kl_ndk.h). An AAsset is a plain file under the assets root —
// the unpacked APK tree, which the JNI side (kl_jni.c) also serves from.
typedef struct { FILE *f; int64_t len; void *buf; char path[1200]; } kl_asset;

static char g_asset_root[1024] = "assets";
void kl_ndk_set_assets_dir(const char *dir) {
    if (dir) snprintf(g_asset_root, sizeof g_asset_root, "%s", dir);
}

static char g_asset_manager;         // one manager; it is a directory, not state

static void *kl_AAssetManager_fromJava(void *env, void *jmgr) {
    (void)env; (void)jmgr;
    return &g_asset_manager;
}

// ...and the same one without a JNI round trip, for ANativeActivity. Android
// fills that struct's assetManager field itself, and it must be the pointer
// AAssetManager_fromJava would return or the guest would hold two managers for
// one directory.
void *kl_ndk_asset_manager(void) { return &g_asset_manager; }

static void *kl_AAssetManager_open(void *mgr, const char *fname, int mode) {
    (void)mgr; (void)mode;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_asset_root, fname);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    kl_asset *a = calloc(1, sizeof *a);
    if (!a) { fclose(f); return NULL; }
    fseeko(f, 0, SEEK_END);
    a->len = ftello(f);
    fseeko(f, 0, SEEK_SET);
    a->f = f;
    snprintf(a->path, sizeof a->path, "%s", path);
    return a;
}

// The whole asset as one pointer. Android can hand back a pointer into the
// mapped APK because an uncompressed asset is contiguous there; an asset here is
// a plain file, so this reads it once and caches the buffer on the handle.
//
// The lifetime is the ASSET's, in both — the buffer dies with AAsset_close and
// the caller is not allowed to free it — so matching that exactly is what keeps
// a leak the guest's rather than ours. UE4 reads UE4CommandLine.txt this way.
static const void *kl_AAsset_getBuffer(kl_asset *a) {
    if (!a || !a->f) return NULL;
    if (a->buf) return a->buf;
    if (a->len < 0) return NULL;
    void *buf = malloc((size_t)a->len + 1);
    if (!buf) return NULL;
    off_t was = ftello(a->f);
    fseeko(a->f, 0, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)a->len, a->f);
    fseeko(a->f, was, SEEK_SET);
    ((char *)buf)[got] = '\0';
    a->buf = buf;
    return a->buf;
}

// ...and the same asset as a file descriptor plus a window into it. Android
// answers with a descriptor onto the APK and a non-zero offset, which is why the
// out-parameters exist at all; here every asset is its own file, so the window
// is the whole of it. Refused for a compressed asset on Android — nothing here
// is compressed, so this never takes that arm.
//
// Both out-parameters are WRITTEN on success and left alone on failure, which is
// trap 10b's rule: a caller that reads an out-parameter we never wrote is
// reading its own uninitialised stack.
static int kl_AAsset_openFileDescriptor(kl_asset *a, int64_t *out_start, int64_t *out_len) {
    if (!a || !a->path[0]) return -1;
    int fd = open(a->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (out_start) *out_start = 0;
    if (out_len)   *out_len   = a->len;
    return fd;
}

static int kl_AAsset_read(kl_asset *a, void *buf, size_t count) {
    if (!a || !a->f) return -1;
    return (int)fread(buf, 1, count, a->f);
}
static int64_t kl_AAsset_getLength64(kl_asset *a) { return a ? a->len : -1; }
static int64_t kl_AAsset_seek64(kl_asset *a, int64_t off, int whence) {
    if (!a || !a->f || fseeko(a->f, off, whence) != 0) return -1;
    return ftello(a->f);
}
static void kl_AAsset_close(kl_asset *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    free(a->buf);
    free(a);
}
// The 32-bit spelling. Both exist in the NDK and the newer SDL3 in the VR build
// imports this one; truncating is what Android does too, and no asset here is
// anywhere near 2 GB.
static int kl_AAsset_getLength(kl_asset *a) { return a ? (int)a->len : -1; }

// Directory enumeration. Android returns FILE names only — no subdirectories,
// no "." or ".." — and returns NULL to end iteration, which is also how the
// caller learns the directory was empty. The name belongs to the AAssetDir and
// is invalidated by the next call, so it is stored in the handle rather than
// strdup'd: a caller that keeps it is relying on Android's lifetime, and
// matching that exactly is what keeps a bug ours rather than theirs.
typedef struct { DIR *d; char name[256]; } kl_assetdir;

static void *kl_AAssetManager_openDir(void *mgr, const char *dirname) {
    (void)mgr;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_asset_root, dirname && *dirname ? dirname : ".");
    DIR *d = opendir(path);
    if (!d) return NULL;
    kl_assetdir *ad = calloc(1, sizeof *ad);
    if (!ad) { closedir(d); return NULL; }
    ad->d = d;
    return ad;
}

static const char *kl_AAssetDir_getNextFileName(kl_assetdir *ad) {
    if (!ad || !ad->d) return NULL;
    for (struct dirent *e; (e = readdir(ad->d)) != NULL; ) {
        if (e->d_name[0] == '.') continue;           // skip . .. and dotfiles
        if (e->d_type == DT_DIR) continue;           // files only, as Android does
        snprintf(ad->name, sizeof ad->name, "%s", e->d_name);
        return ad->name;
    }
    return NULL;
}

static void kl_AAssetDir_close(kl_assetdir *ad) {
    if (!ad) return;
    if (ad->d) closedir(ad->d);
    free(ad);
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

// ============================================================ AConfiguration
// An AConfiguration is Android's ResTable_config — locale, orientation, density,
// screen size, the fields resource selection is done with. The VR guest imports
// exactly three of the family, and none of them is a getter:
//
//   AConfiguration_new / _fromAssetManager / _delete
//
// which is native_app_glue's boilerplate verbatim. So nothing ever reads a field
// back, and the honest answer is a zeroed block: every ACONFIGURATION_*_ANY
// constant is 0, so "all defaults, nothing specified" is what the guest sees.
// That is also true — we have no Android resource configuration to report.
//
// Sized generously and not from a transcribed struct, deliberately: the layout
// is private to the platform, we hand out the only pointers, and the moment a
// getter appears in an import list it becomes an unresolved name that stops the
// run BY NAME rather than a field read off the end of a struct we guessed at.
#define KL_ACONFIG_BYTES 128

static void *kl_AConfiguration_new(void) { return calloc(1, KL_ACONFIG_BYTES); }
static void  kl_AConfiguration_delete(void *c) { free(c); }
static void  kl_AConfiguration_fromAssetManager(void *c, void *mgr) {
    // Android fills the config from the asset manager's current device state.
    // Ours has none, so this stays all-defaults; it is not a no-op standing in
    // for something, it is the whole answer.
    (void)mgr;
    if (c) memset(c, 0, KL_ACONFIG_BYTES);
}

// ...and the getters, which the note above predicted would arrive one day and
// stop a run BY NAME. **Unreal Engine 4 is that day**: native_app_glue's
// android_main reads the locale straight out of the configuration, and RE4 dies
// in ANativeActivity_onCreate without them.
//
// The answer is kl_jni's, not a second opinion — java.util.Locale and
// AConfiguration are two doors onto one fact, and UE4 reads both (Locale for
// FInternationalization, this for its resource configuration). See
// kl_jni_locale_parts.
//
// THE OUTPUT IS NOT A C STRING. `AConfiguration_getLanguage` fills exactly two
// characters and writes no terminator — the NDK header's own wording is "the
// two-character array", and callers pass a `char[2]` on the stack. Writing a
// third byte is a stack smash in the guest, and it would be silent on the
// happy path and lethal on an unlucky frame layout. Zero-filled when there is
// no language, which is what Android does and what every ACONFIGURATION_*_ANY
// default already says.
static void kl_AConfiguration_getLanguage(void *c, char *out) {
    (void)c;
    if (!out) return;
    char lang[16], country[16];
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    out[0] = lang[0] ? lang[0] : 0;
    out[1] = lang[0] && lang[1] ? lang[1] : 0;
}
static void kl_AConfiguration_getCountry(void *c, char *out) {
    (void)c;
    if (!out) return;
    char lang[16], country[16];
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    out[0] = country[0] ? country[0] : 0;
    out[1] = country[0] && country[1] ? country[1] : 0;
}

// ========================================================== ANativeActivity
// The two calls the guest makes back INTO the activity. Both are requests to
// the framework, not queries, so there is nothing to answer — but neither is a
// no-op in meaning, and a silent one would be trap 6d.
static void kl_ANativeActivity_finish(void *act) {
    (void)act;
    fprintf(stderr, "  [ndk] ANativeActivity_finish() — the guest is asking to "
                    "close the activity\n");
}
// The window's pixel format, asked for before the window exists. UE4 calls this
// from its glue as the first thing after onStart, with one of the
// WINDOW_FORMAT_* constants (RGBA_8888 = 1, RGBX_8888 = 2, RGB_565 = 4).
//
// Recorded rather than applied, and that is not a shrug: the surface the guest
// actually renders into here is ANGLE's, whose format is settled by the EGL
// config the guest itself chooses through eglChooseConfig — so honouring this
// would mean overriding a later, more specific request with an earlier, vaguer
// one. Android's own behaviour is close to that: setWindowFormat is a HINT to
// the window manager and the EGL config still wins for what gets drawn.
//
// Named on the way through, because a format we ignore is exactly the thing to
// know about if a guest's colours ever come out wrong.
static void kl_ANativeActivity_setWindowFormat(void *act, int format) {
    (void)act;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [ndk] ANativeActivity_setWindowFormat(%d) — recorded, "
                        "not applied: the EGL config the guest chooses decides "
                        "the surface format here\n", format);
    }
}
static void kl_ANativeActivity_showSoftInput(void *act, unsigned flags) {
    (void)act;
    fprintf(stderr, "  [ndk] ANativeActivity_showSoftInput(0x%x) — no soft "
                    "keyboard here\n", flags);
}

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
    N("ANativeWindow_lock",               kl_ANativeWindow_lock),
    N("ANativeWindow_unlockAndPost",      kl_ANativeWindow_unlockAndPost),

    N("AAssetManager_fromJava", kl_AAssetManager_fromJava),
    N("AAssetManager_open",     kl_AAssetManager_open),
    N("AAsset_read",            kl_AAsset_read),
    N("AAsset_getLength64",     kl_AAsset_getLength64),
    N("AAsset_seek64",          kl_AAsset_seek64),
    N("AAsset_close",           kl_AAsset_close),
    N("AAsset_getLength",       kl_AAsset_getLength),
    N("AAsset_getBuffer",       kl_AAsset_getBuffer),
    N("AAsset_openFileDescriptor", kl_AAsset_openFileDescriptor),
    N("AAssetManager_openDir",  kl_AAssetManager_openDir),
    N("AAssetDir_getNextFileName", kl_AAssetDir_getNextFileName),
    N("AAssetDir_close",        kl_AAssetDir_close),

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

    N("AConfiguration_new",              kl_AConfiguration_new),
    N("AConfiguration_delete",           kl_AConfiguration_delete),
    N("AConfiguration_fromAssetManager", kl_AConfiguration_fromAssetManager),
    N("AConfiguration_getLanguage",      kl_AConfiguration_getLanguage),
    N("AConfiguration_getCountry",       kl_AConfiguration_getCountry),

    N("ANativeActivity_finish",        kl_ANativeActivity_finish),
    N("ANativeActivity_setWindowFormat", kl_ANativeActivity_setWindowFormat),
    N("ANativeActivity_showSoftInput", kl_ANativeActivity_showSoftInput),
#undef N
};

void *kl_ndk_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_ndk / sizeof g_ndk[0]; i++)
        if (strcmp(g_ndk[i].name, name) == 0) return g_ndk[i].fn;
    return NULL;
}
