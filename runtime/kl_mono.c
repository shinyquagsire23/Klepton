// See kl_mono.h. Lifted out of kl_view.c when the visionOS app became a second
// window frontend for the same guest.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kl_mono.h"
#include "kl_jni.h"
#include "kl_glfb.h"
#include "kl_present.h"

#define KL_MONO_SDLA "org/libsdl/app/SDLActivity"

typedef void (*kl_fn_mouse)(void *env, void *cls, int32_t state, int32_t action,
                            float x, float y, uint8_t relative);
typedef void (*kl_fn_key)(void *env, void *cls, int32_t keycode);

static struct {
    int         resolved;
    kl_fn_mouse mouse;
    kl_fn_key   key_down, key_up;
    void       *env, *cls;
} g_in;

// Lazily, because RegisterNatives runs long after this file's first line and a
// frontend exists before the guest does.
static void mono_resolve(void) {
    if (g_in.resolved) return;
    g_in.resolved = 1;
    g_in.mouse    = (kl_fn_mouse)kl_jni_native(KL_MONO_SDLA, "onNativeMouse", NULL);
    g_in.key_down = (kl_fn_key)kl_jni_native(KL_MONO_SDLA, "onNativeKeyDown", NULL);
    g_in.key_up   = (kl_fn_key)kl_jni_native(KL_MONO_SDLA, "onNativeKeyUp", NULL);
    if (!g_in.mouse) {
        fprintf(stderr, "  [mono] the guest registered no %s.onNativeMouse — "
                        "the window is display-only\n", KL_MONO_SDLA);
        return;
    }
    g_in.env = kl_jni_env();
    g_in.cls = kl_jni_class(KL_MONO_SDLA);
    fprintf(stderr, "  [mono] pointer and keys go to the guest\n");
}

int kl_mono_input_available(void) {
    mono_resolve();
    return g_in.mouse != NULL;
}

void kl_mono_pointer(int state, int action, float x, float y) {
    mono_resolve();
    if (!g_in.mouse) return;
    kl_jni_local_frame_push();
    g_in.mouse(g_in.env, g_in.cls, (int32_t)state, (int32_t)action, x, y, 0);
    kl_jni_local_frame_pop();
}

void kl_mono_key(int down, int keycode) {
    mono_resolve();
    kl_fn_key fn = down ? g_in.key_down : g_in.key_up;
    if (!fn || !keycode) return;
    kl_jni_local_frame_push();
    fn(g_in.env, g_in.cls, (int32_t)keycode);
    kl_jni_local_frame_pop();
}

// Only what a configuration UI is driven with: navigation, activation,
// dismissal, and enough of a keyboard to type a host address or a PIN.
int kl_mono_keycode_for_char(int ch) {
    if (ch >= 'a' && ch <= 'z') return 29 + (ch - 'a');   // KEYCODE_A .. KEYCODE_Z
    if (ch >= 'A' && ch <= 'Z') return 29 + (ch - 'A');
    if (ch >= '1' && ch <= '9') return 8 + (ch - '1');    // KEYCODE_1 .. KEYCODE_9
    switch (ch) {
    case '0':    return 7;    // KEYCODE_0
    case '\r':
    case '\n':   return 66;   // KEYCODE_ENTER
    case '\b':
    case 0x7f:   return 67;   // KEYCODE_DEL
    case '\t':   return 61;   // KEYCODE_TAB
    case ' ':    return 62;   // KEYCODE_SPACE
    case '.':    return 56;   // KEYCODE_PERIOD
    case ',':    return 55;   // KEYCODE_COMMA
    case '-':    return 69;   // KEYCODE_MINUS
    // Escape is the BACK button, not KEYCODE_ESCAPE: that is what a headset or
    // a phone gives this app, and Steam Link's own manifest
    // (SDL_ANDROID_TRAP_BACK_BUTTON) says it handles the back button itself.
    case 0x1b:   return 4;    // KEYCODE_BACK
    default:     return 0;
    }
}

// --- frame out --------------------------------------------------------------
//
// Two buffers rather than one, because the sink runs INSIDE the guest's frame:
// it takes the lock only to swap a pointer, so a consumer holding the front
// buffer for the length of a texture upload cannot stall a guest frame for that
// long. Reallocated only when the size changes, which for a flat guest is once.
static struct {
    pthread_mutex_t mu;
    uint8_t   *front, *back;
    size_t     cap;
    int        w, h;
    uint32_t   serial;
    int        started;
} g_fb = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void mono_sink(const uint8_t *rgba, int w, int h, void *ctx) {
    (void)ctx;
    if (!rgba || w <= 0 || h <= 0) return;
    size_t need = (size_t)w * (size_t)h * 4;

    pthread_mutex_lock(&g_fb.mu);
    if (g_fb.cap < need) {
        uint8_t *a = realloc(g_fb.front, need);
        uint8_t *b = realloc(g_fb.back, need);
        if (!a || !b) { pthread_mutex_unlock(&g_fb.mu); return; }
        g_fb.front = a; g_fb.back = b; g_fb.cap = need;
    }
    uint8_t *dst = g_fb.back;
    pthread_mutex_unlock(&g_fb.mu);

    memcpy(dst, rgba, need);

    pthread_mutex_lock(&g_fb.mu);
    // Only if nothing reallocated underneath us — otherwise the copy above went
    // to a buffer that is no longer the back one, and publishing it would show
    // whatever realloc left behind.
    if (dst == g_fb.back) {
        g_fb.back = g_fb.front;
        g_fb.front = dst;
        g_fb.w = w; g_fb.h = h;
        g_fb.serial++;
    }
    pthread_mutex_unlock(&g_fb.mu);
}

void kl_mono_capture_start(void) {
    if (g_fb.started) return;
    g_fb.started = 1;
    kl_glfb_set_frame_sink(mono_sink, NULL);
}

int kl_mono_frame_lock(const uint8_t **rgba, int *w, int *h, uint32_t *serial) {
    pthread_mutex_lock(&g_fb.mu);
    if (!g_fb.serial || !g_fb.front) { pthread_mutex_unlock(&g_fb.mu); return 0; }
    if (rgba)   *rgba   = g_fb.front;
    if (w)      *w      = g_fb.w;
    if (h)      *h      = g_fb.h;
    if (serial) *serial = g_fb.serial;
    return 1;   // still locked — kl_mono_frame_unlock releases it
}

void kl_mono_frame_unlock(void) {
    pthread_mutex_unlock(&g_fb.mu);
}

uint32_t kl_mono_frame_count(void) { return g_fb.serial; }

// --- the synthetic click sequence (KL_VIEW_POKE) -----------------------------
#define KL_MONO_POKE_MAX 12

static uint64_t mono_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void kl_mono_poke_tick(void) {
    static int      parsed;
    static struct { float fx, fy; unsigned delay_ms; } poke[KL_MONO_POKE_MAX];
    static int      npoke, cur, step;
    static uint64_t t_zero, t_step;

    if (parsed && cur >= npoke) return;
    if (!parsed) {
        parsed = 1;
        const char *s = getenv("KL_VIEW_POKE");
        while (s && *s && npoke < KL_MONO_POKE_MAX) {
            float fx, fy; unsigned secs = 0;
            if (sscanf(s, "%f,%f@%u", &fx, &fy, &secs) < 2) break;
            poke[npoke].fx = fx; poke[npoke].fy = fy;
            poke[npoke].delay_ms = secs * 1000;
            npoke++;
            const char *next = strchr(s, ';');
            s = next ? next + 1 : NULL;
        }
        if (npoke)
            fprintf(stderr, "  [mono] KL_VIEW_POKE: %d click%s queued\n",
                    npoke, npoke == 1 ? "" : "s");
        if (cur >= npoke) return;
    }

    // Zero is the MONO TRANSITION — the moment a window surface exists — not
    // this function's first call: a click before the guest has a surface lands
    // on nothing, and how long the chain takes to get there is exactly the
    // thing that varies between a host run and a device one.
    //
    // Off kl_present rather than off the frame store below, because the two
    // frontends fill that store differently: kl_view registers its own kl_glfb
    // sink and never touches kl_mono's, so a size read from here would be zero
    // forever on the host and KL_VIEW_POKE would silently do nothing.
    int w = 0, h = 0;
    kl_present_mono_size(&w, &h);
    if (w <= 0 || h <= 0) return;

    uint64_t now = mono_now_ms();
    if (!t_zero) t_zero = now;
    if (now - t_zero < poke[cur].delay_ms) return;

    // Three steps on three different frames, not three calls in a row. A real
    // click hovers, holds for ~100 ms and releases, and the guest's event loop
    // has to RUN in between: pressed and released within one iteration is a
    // click Qt can sample as never having happened (measured — the button took
    // its highlight and nothing else). The hover step is separate for the same
    // reason: Qt decides what a press lands on from where the pointer already
    // is.
    if (step && now - t_step < 150) return;
    t_step = now;

    float x = poke[cur].fx * (float)w, y = poke[cur].fy * (float)h;
    switch (step++) {
    case 0:
        fprintf(stderr, "  [mono] poke %d/%d click at %.0f,%.0f of %dx%d (t+%.1fs)\n",
                cur + 1, npoke, (double)x, (double)y, w, h,
                (double)(now - t_zero) / 1000.0);
        kl_mono_pointer(0, KL_MONO_HOVER_MOVE, x, y);
        break;
    case 1:
        kl_mono_pointer(KL_MONO_BTN_PRIMARY, KL_MONO_DOWN, x, y);
        break;
    default:
        kl_mono_pointer(0, KL_MONO_UP, x, y);
        step = 0;
        cur++;
        break;
    }
}
