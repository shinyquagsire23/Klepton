// The interactive one-eye viewer — the null frontend's opposite number.
//
// KL_VIEW=1 turns t_boot into a windowed app: the guest runs on a spawned
// thread exactly as it does in the re-exec'd recon child, and the main thread
// runs this SDL loop. The two halves meet at the seams, nowhere else:
//
//   pose in    — WASD + mouse-look here recomputes a head pose every frame and
//                calls kl_ovrp_set_head_pose(); the guest reads it back out of
//                ovrp_GetNodePoseState. On visionOS this same seam is where
//                ARKit's WorldTrackingProvider attaches.
//   frame out  — two implementations of one seam, chosen by t_boot:
//                * hardware (the default). The guest's eye textures ARE
//                  MTLTextures we allocated, and kl_view_mtl.m composites one
//                  into the window's CAMetalLayer. Nothing is read back and
//                  nothing is copied. This is the same pass, and the same
//                  shader, as KleptonCompositor.swift on visionOS.
//                * readback (KL_VIEW_CPU=1). kl_glfb glReadPixels the eye and
//                  hands the buffer to kl_view_frame_sink, which stores it
//                  under a mutex; this loop row-flips it and uploads it to a
//                  streaming texture. Kept because it works with no Metal
//                  interop at all, which makes it the A/B when the hardware
//                  path shows the wrong picture.
//
// Everything in this file is host-only scaffolding, like kl_glfb itself: it
// exists so a human can look at the frame and walk the pose around while the
// M6 problems (no scene draws, the Bloom abort) are worked.
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kl_view.h"
#include "kl_glfb.h"     // kl_glfb_last_frame_lit — the HUD's liveness signal
#include "kl_mono.h"     // the flat guest's input path, shared with the app
#include "kl_view_mtl.h" // the hardware composite path
#include "kl_ovrp.h"     // kl_ovrp_set_head_pose — the pose-in seam
#include "kl_present.h"  // mono vs stereo — which shape of picture the guest makes
#include "kl_jni.h"      // the mono input path: SDLActivity's registered natives
#include "kl_env.h"

#if __has_include(<SDL3/SDL.h>)
#define KL_VIEW_HAVE_SDL 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#endif

// ---- axis and control conventions -------------------------------------------
// OVR space: +x is right, +y is up, forward is −z. Yaw rotates about +y with
// yaw=0 facing −z, pitch about +x with pitch>0 looking up; mouse right and
// mouse up turn the view right and up. THESE SIGNS are the empirical flip
// point: if the window ever shows the view turning or walking backwards, the
// change belongs to one of these constants, not to the math below.
#define KL_VIEW_MOUSE_RAD_PER_PX 0.0025f   // radians of turn per pixel of mouse
#define KL_VIEW_MOVE_SPEED       2.0f      // metres per second, in the yaw plane
#define KL_VIEW_PITCH_CLAMP      1.5f      // just under straight up/down (~86°)
#define KL_VIEW_EYE_HEIGHT       1.6f      // standing eye height, metres
#define KL_VIEW_WIN_W            920       // half of 1832x1920, roughly one eye
#define KL_VIEW_WIN_H            960
// The smallest guest panel worth resizing the window to. Below this it is a
// placeholder surface rather than a picture — see the mono transition.
#define KL_VIEW_MONO_MIN         64

// The double-buffered frame store, collapsed to the latest frame: the sink
// overwrites it under the mutex and sets the flag, the SDL loop consumes and
// clears it. Dropping intermediate frames is deliberate — the display is
// 60 Hz, the guest is 72, and a queue would only add latency. The buffer is
// bottom-up RGBA exactly as glReadPixels produced it; the row flip happens on
// the consuming side so the sink stays a pure memcpy.
static pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *g_frame_buf;
static size_t          g_frame_cap;
static int             g_frame_w, g_frame_h;
static int             g_frame_new;
static unsigned        g_frame_seq;   // total frames the sink has stored

void kl_view_frame_sink(const uint8_t *rgba, int w, int h, void *ctx) {
    (void)ctx;
    // On the GL thread inside the guest's frame — memcpy and go. No SDL calls
    // here, ever: SDL is owned by the main thread.
    size_t n = (size_t)w * (size_t)h * 4;
    pthread_mutex_lock(&g_frame_mu);
    if (n > g_frame_cap) {
        uint8_t *nb = realloc(g_frame_buf, n);
        if (!nb) { pthread_mutex_unlock(&g_frame_mu); return; }
        g_frame_buf = nb;
        g_frame_cap = n;
    }
    memcpy(g_frame_buf, rgba, n);
    g_frame_w = w;
    g_frame_h = h;
    g_frame_new = 1;
    g_frame_seq++;
    pthread_mutex_unlock(&g_frame_mu);
}

int kl_view_available(void) {
#ifdef KL_VIEW_HAVE_SDL
    return 1;
#else
    return 0;
#endif
}

#ifdef KL_VIEW_HAVE_SDL

// ---- mono input: the window's pointer and keys, into the guest ---------------
//
// A flat guest has no pose to drive, so the mouse means what it means on a
// desktop: it is the pointer. The route into the guest is runtime/kl_mono.c —
// its OWN Android input path, `SDLActivity.onNativeMouse` / `onNativeKeyDown` /
// `onNativeKeyUp`, which SDL3 registered with us at JNI_OnLoad. That moved out
// of this file when the visionOS app became a second window frontend for the
// same guest: two frontends inventing the same Android events separately is two
// chances to invent them differently.
//
// What stays here is the part that is genuinely SDL's — the window-to-surface
// scaling and the scancode table.
//
// The MotionEvent constants are kl_mono.h's (KL_MONO_*); these aliases keep the
// call sites in this file reading the way they always have.
#define KL_AMOTION_DOWN        KL_MONO_DOWN
#define KL_AMOTION_UP          KL_MONO_UP
#define KL_AMOTION_MOVE        KL_MONO_MOVE
#define KL_AMOTION_HOVER_MOVE  KL_MONO_HOVER_MOVE
#define KL_AMOTION_SCROLL      KL_MONO_SCROLL
#define KL_ABUTTON_PRIMARY     KL_MONO_BTN_PRIMARY
#define KL_ABUTTON_SECONDARY   KL_MONO_BTN_SECONDARY
#define KL_ABUTTON_TERTIARY    KL_MONO_BTN_TERTIARY

// Window points -> the guest's surface pixels. The window is resized to the
// guest's panel on the mode transition, so this is usually 1:1 — but only
// usually: a Retina drawable and a user-resized window both break it, and a
// pointer that lands in the wrong place is the kind of bug that reads as "the
// UI ignores clicks".
static void view_mono_scale(SDL_Window *win, float *x, float *y) {
    int ww = 0, wh = 0, gw = 0, gh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    kl_present_mono_size(&gw, &gh);
    if (ww > 0 && gw > 0) *x = *x * (float)gw / (float)ww;
    if (wh > 0 && gh > 0) *y = *y * (float)gh / (float)wh;
}

static void view_mono_mouse(SDL_Window *win, int32_t state, int32_t action,
                            float x, float y) {
    if (!kl_mono_input_available()) return;
    view_mono_scale(win, &x, &y);
    kl_mono_pointer(state, action, x, y);
}

// SDL scancode -> Android keycode. Only what a configuration UI is driven with:
// navigation, activation, dismissal, and enough of a keyboard to type a host
// address or a PIN. Anything absent is dropped rather than guessed at — a wrong
// keycode is a keypress the guest acts on, which is worse than none.
static int32_t view_mono_keycode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 29 + (sc - SDL_SCANCODE_A);            // KEYCODE_A .. KEYCODE_Z
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 8 + (sc - SDL_SCANCODE_1);             // KEYCODE_1 .. KEYCODE_9
    switch (sc) {
    case SDL_SCANCODE_0:          return 7;           // KEYCODE_0
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:   return 66;          // KEYCODE_ENTER
    case SDL_SCANCODE_BACKSPACE:  return 67;          // KEYCODE_DEL
    case SDL_SCANCODE_TAB:        return 61;          // KEYCODE_TAB
    case SDL_SCANCODE_SPACE:      return 62;          // KEYCODE_SPACE
    case SDL_SCANCODE_UP:         return 19;          // KEYCODE_DPAD_UP
    case SDL_SCANCODE_DOWN:       return 20;          // KEYCODE_DPAD_DOWN
    case SDL_SCANCODE_LEFT:       return 21;          // KEYCODE_DPAD_LEFT
    case SDL_SCANCODE_RIGHT:      return 22;          // KEYCODE_DPAD_RIGHT
    case SDL_SCANCODE_PERIOD:     return 56;          // KEYCODE_PERIOD
    case SDL_SCANCODE_COMMA:      return 55;          // KEYCODE_COMMA
    case SDL_SCANCODE_MINUS:      return 69;          // KEYCODE_MINUS
    // Escape is the BACK button, not KEYCODE_ESCAPE: that is what a headset or
    // a phone gives this app, and the manifest's own SDL_ANDROID_TRAP_BACK_BUTTON
    // says Steam Link handles it itself.
    case SDL_SCANCODE_ESCAPE:     return 4;           // KEYCODE_BACK
    default:                      return 0;
    }
}

// KL_VIEW_POKE lives in runtime/kl_mono.c now — the fractions are of the
// guest's own surface, which both frontends scale into, so one implementation
// serves the SDL window and the visionOS one and a click means the same thing
// on each. See kl_mono.h for what it is for.

static void view_mono_key(int down, SDL_Scancode sc) {
    kl_mono_key(down, view_mono_keycode(sc));
}

// ---- the readback path's display (KL_VIEW_CPU=1) ----------------------------
// Everything in here is what the hardware path does not do: a row flip of the
// whole eye, an upload of the whole eye, and upstream of both a glReadPixels
// that stalled the guest's GL thread to produce the buffer. Kept as the A/B —
// it needs no Metal interop, so a picture that is right here and wrong through
// the compositor localises the bug to the compositor.
typedef struct {
    SDL_Texture *tex;              // created on the first frame, size unknown till then
    int          tex_w, tex_h;
    uint8_t     *flip;             // top-down staging for SDL_UpdateTexture
    size_t       flip_cap;
} view_cpu_disp;

static void view_show_cpu_frame(view_cpu_disp *d, SDL_Renderer *ren, SDL_Window *win) {
    // Newest frame, if the sink stored one since last time: flip GL's
    // bottom-up rows into top-down and upload. SDL_PIXELFORMAT_RGBA32 is
    // the byte-order alias whose memory layout is R,G,B,A — exactly what
    // the glReadPixels(GL_RGBA) in kl_glfb produced.
    pthread_mutex_lock(&g_frame_mu);
    int have = g_frame_new, w = g_frame_w, h = g_frame_h;
    if (have) {
        size_t n = (size_t)w * (size_t)h * 4;
        if (n > d->flip_cap) {
            uint8_t *nb = realloc(d->flip, n);
            if (nb) { d->flip = nb; d->flip_cap = n; }
        }
        if (d->flip) {
            size_t stride = (size_t)w * 4;
            for (int y = 0; y < h; y++)
                memcpy(d->flip + stride * (size_t)y,
                       g_frame_buf + stride * (size_t)(h - 1 - y), stride);
        } else {
            have = 0;
        }
        g_frame_new = 0;
    }
    pthread_mutex_unlock(&g_frame_mu);

    if (have) {
        if (!d->tex || w != d->tex_w || h != d->tex_h) {
            if (d->tex) SDL_DestroyTexture(d->tex);
            d->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
            d->tex_w = w;
            d->tex_h = h;
        }
        if (d->tex) SDL_UpdateTexture(d->tex, NULL, d->flip, w * 4);
    }

    if (d->tex) {
        // Letterbox: the eye is 1832x1920-ish, the window is not.
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        float scale = fminf((float)ww / d->tex_w, (float)wh / d->tex_h);
        SDL_FRect dst = {
            (ww - d->tex_w * scale) * 0.5f, (wh - d->tex_h * scale) * 0.5f,
            d->tex_w * scale, d->tex_h * scale,
        };
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, d->tex, NULL, &dst);
        SDL_RenderPresent(ren);
    }
}

static void view_cpu_disp_free(view_cpu_disp *d) {
    free(d->flip);
    if (d->tex) SDL_DestroyTexture(d->tex);
    d->flip = NULL;
    d->tex = NULL;
}

int kl_view_main(const char *libdir, int hw) {
    (void)libdir;   // the title names the frontend, not the target
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "view: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Klepton", KL_VIEW_WIN_W, KL_VIEW_WIN_H,
                                       hw ? SDL_WINDOW_METAL : 0);
    if (!win) {
        fprintf(stderr, "view: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // The two frame paths are mutually exclusive at the window level: the
    // hardware one owns the window's CAMetalLayer directly, so there is no
    // SDL_Renderer to create — an SDL_Renderer would want the same layer.
    SDL_Renderer *ren = NULL;
    SDL_MetalView mview = NULL;
    void *mlayer = NULL;
    if (hw) {
        mview = SDL_Metal_CreateView(win);
        mlayer = mview ? SDL_Metal_GetLayer(mview) : NULL;
        if (!mlayer) {
            fprintf(stderr, "view: SDL_Metal_CreateView failed (%s) — falling "
                            "back to the readback path\n", SDL_GetError());
            if (mview) SDL_Metal_DestroyView(mview);
            mview = NULL;
            kl_glfb_set_frame_sink(kl_view_frame_sink, NULL);
            hw = 0;
        }
    }
    if (!hw) {
        ren = SDL_CreateRenderer(win, NULL);
        if (!ren) {
            fprintf(stderr, "view: SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }

    // Head state. Position starts at standing eye height; yaw/pitch start at
    // zero, facing −z per the convention block above.
    float px = 0, py = KL_VIEW_EYE_HEIGHT, pz = 0;
    float yaw = 0, pitch = 0;
    int mouselook = 0;
    int mouse_l = 0, mouse_r = 0;   // right-hand trigger / grip
    int32_t mono_buttons = 0;       // the flat path's Android button-state mask
    uint64_t t_mono = 0;            // when the guest went mono — KL_VIEW_POKE's zero

    view_cpu_disp disp = {0};      // the readback path's staging; unused when hw
    int hw_up = 0, hw_warned = 0;  // the compositor starts lazily — see the loop

    // Which SHAPE of picture the guest is producing (kl_present.h). This cannot
    // be decided before the loop: the guest runs on another thread and has not
    // created its window surface or its eye textures yet when we get here, so
    // the mode is NONE for the first frames of every run. Hence the generation
    // check inside the loop rather than a query outside it — and hence
    // kl_present exposing a generation at all.
    //
    // Mono is a flat app (Steam Link): no pose to drive, no stereo, and the
    // readback sink is the whole frame path. Driving kl_ovrp in that mode would
    // be writing poses nothing will ever read, and the HUD would report a head
    // position for an app that has no head.
    unsigned pres_gen = ~0u;
    int mono = 0;

    int done = 0;
    uint64_t t_prev = SDL_GetTicks();
    uint64_t t_start = t_prev, hud_last = t_prev;
    while (!done) {
        uint64_t t_now = SDL_GetTicks();
        float dt = (float)(t_now - t_prev) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;  // a stall is not a teleport
        t_prev = t_now;

        // Act on a mode TRANSITION, once. On macOS that is only a window resize
        // and which seams to drive; on visionOS the same edge is where a window
        // would be opened or an immersive space dismissed, which is why the
        // signal is a generation and not a polled value (kl_present.h).
        if (kl_present_generation() != pres_gen) {
            pres_gen = kl_present_generation();
            kl_present_mode m = kl_present_mode_now();
            mono = (m == KL_PRESENT_MONO);
            if (mono) {
                // Match the window to the guest's panel so its picture is shown
                // 1:1 rather than letterboxed into a VR-shaped window.
                // ...but only for a panel a person could look at. A Unity XR
                // guest brings up a placeholder surface before the XR display
                // starts — VRChat's is 16x16 — and matching the window to it
                // leaves a window too small to see, which reads as "the viewer
                // is broken" rather than "the guest has not started XR yet".
                int mw = 0, mh = 0;
                kl_present_mono_size(&mw, &mh);
                if (mw >= KL_VIEW_MONO_MIN && mh >= KL_VIEW_MONO_MIN) {
                    SDL_SetWindowSize(win, mw, mh);
                    SDL_SetWindowTitle(win, "Klepton — 2D guest");
                } else if (mw > 0 && mh > 0) {
                    fprintf(stderr, "view: the guest's panel is %dx%d — too small "
                                    "to be a picture; keeping the window\n", mw, mh);
                }
                fprintf(stderr, "view: guest is MONO %dx%d — flat window, no pose\n",
                        mw, mh);
                // Resolve the input natives HERE rather than on the first
                // event: by the mode transition the guest has run JNI_OnLoad
                // (that is what created the window surface), so the lookup can
                // succeed, and reporting it now means every run says whether
                // the window is interactive instead of only the runs someone
                // moved the mouse in.
                (void)kl_mono_input_available();
                t_mono = t_now;
            } else if (m == KL_PRESENT_STEREO) {
                // Symmetric with the mono branch, and it had no counterpart:
                // a guest that goes mono and THEN stereo (every Unity OpenXR
                // title — the placeholder surface comes first) was left in a
                // window sized for the panel it has stopped drawing.
                SDL_SetWindowSize(win, KL_VIEW_WIN_W, KL_VIEW_WIN_H);
                SDL_SetWindowTitle(win, "Klepton — VR guest (one eye)");
                fprintf(stderr, "view: guest is STEREO — driving pose and hands\n");
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Mono and stereo want OPPOSITE things from the same events. A VR
            // guest wants the mouse captured and read as relative turn; a flat
            // one wants an ordinary absolute pointer it can click a button
            // with, and capturing it would hide the cursor over a UI that has
            // one. So the split is here rather than inside each case.
            if (mono) {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                    done = 1;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    int down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    int bit = ev.button.button == SDL_BUTTON_RIGHT  ? KL_ABUTTON_SECONDARY
                            : ev.button.button == SDL_BUTTON_MIDDLE ? KL_ABUTTON_TERTIARY
                                                                    : KL_ABUTTON_PRIMARY;
                    if (down) mono_buttons |= bit; else mono_buttons &= ~bit;
                    view_mono_mouse(win, mono_buttons,
                                    down ? KL_AMOTION_DOWN : KL_AMOTION_UP,
                                    ev.button.x, ev.button.y);
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION:
                    // Dragging is MOVE, hovering is HOVER_MOVE — Qt needs the
                    // hover half to light up a button before it is clicked.
                    view_mono_mouse(win, mono_buttons,
                                    mono_buttons ? KL_AMOTION_MOVE : KL_AMOTION_HOVER_MOVE,
                                    ev.motion.x, ev.motion.y);
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    view_mono_mouse(win, mono_buttons, KL_AMOTION_SCROLL,
                                    ev.wheel.x, ev.wheel.y);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    view_mono_key(1, ev.key.scancode);
                    break;
                case SDL_EVENT_KEY_UP:
                    view_mono_key(0, ev.key.scancode);
                    break;
                }
                continue;
            }
            switch (ev.type) {
            case SDL_EVENT_QUIT:                       // includes Cmd-Q
                done = 1;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                // Click captures the mouse; Esc releases it. Relative mode is
                // what makes xrel/yrel meaningful for look.
                if (!mouselook) {
                    SDL_SetWindowRelativeMouseMode(win, true);
                    mouselook = 1;
                }
                if (ev.button.button == SDL_BUTTON_LEFT)  mouse_l = 1;
                if (ev.button.button == SDL_BUTTON_RIGHT) mouse_r = 1;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT)  mouse_l = 0;
                if (ev.button.button == SDL_BUTTON_RIGHT) mouse_r = 0;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouselook) {
                    yaw   -= ev.motion.xrel * KL_VIEW_MOUSE_RAD_PER_PX;
                    pitch -= ev.motion.yrel * KL_VIEW_MOUSE_RAD_PER_PX;
                    if (pitch >  KL_VIEW_PITCH_CLAMP) pitch =  KL_VIEW_PITCH_CLAMP;
                    if (pitch < -KL_VIEW_PITCH_CLAMP) pitch = -KL_VIEW_PITCH_CLAMP;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE && mouselook) {
                    SDL_SetWindowRelativeMouseMode(win, false);
                    mouselook = 0;
                }
                break;
            }
        }

        // Movement, in the yaw plane: forward is (−sin yaw, 0, −cos yaw) and
        // right is (cos yaw, 0, −sin yaw) under the convention above. R/Space
        // rise, F/Shift sink — vertical is not yaw-relative, it is +y itself.
        const bool *keys = SDL_GetKeyboardState(NULL);
        float sy = sinf(yaw), cy = cosf(yaw);
        float mx = 0, mz = 0;
        if (keys[SDL_SCANCODE_W]) { mx -= sy; mz -= cy; }
        if (keys[SDL_SCANCODE_S]) { mx += sy; mz += cy; }
        if (keys[SDL_SCANCODE_D]) { mx += cy; mz -= sy; }
        if (keys[SDL_SCANCODE_A]) { mx -= cy; mz += sy; }
        px += mx * KL_VIEW_MOVE_SPEED * dt;
        pz += mz * KL_VIEW_MOVE_SPEED * dt;
        if (keys[SDL_SCANCODE_R] || keys[SDL_SCANCODE_SPACE])
            py += KL_VIEW_MOVE_SPEED * dt;
        if (keys[SDL_SCANCODE_F] || keys[SDL_SCANCODE_LSHIFT])
            py -= KL_VIEW_MOVE_SPEED * dt;

        // Pose out through the seam, every frame, even when nothing moved —
        // the read side tolerates a torn read (kl_ovrp.c says why), and a
        // constant rewrite keeps the frames honest.
        float hqx, hqy, hqz, hqw;
        {
            float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
            float syw = sinf(yaw * 0.5f), cyw = cosf(yaw * 0.5f);
            // q = yaw ⊗ pitch: quat multiply of (0,syw,0,cyw) and (sp,0,0,cp).
            hqx = cyw * sp; hqy = cp * syw; hqz = -syw * sp; hqw = cyw * cp;
            if (!mono) kl_ovrp_set_head_pose(px, py, pz, hqx, hqy, hqz, hqw);
        }
        // M7 controller emulation — VR only; a flat guest has no hands.
        // Both hands ride head-relative offsets
        // (rotated by the head quat) with the head's orientation — a menu
        // pointer, not a saber sim. ovrpButton bits (guest metadata):
        // One=0x1 Two=0x2 Three=0x4 Four=0x8; Primary(right): IndexTrigger
        // 0x2000, HandTrigger 0x4000, Thumbstick 0x8000, stick U/D/L/R
        // 0x10000/0x20000/0x40000/0x80000; Secondary(left): the same <<10
        // (0x200000/0x400000/0x800000, stick dirs 0x1000000..0x8000000).
        // Mouse L = right trigger, mouse R = right grip, Z/X = A/B, C/V =
        // X/Y, G/H = left trigger/grip, arrows = right thumbstick.
        if (!mono) {
            // KL_VIEW_AIM_AT_EYE=1 collapses both hands onto the head
            // position. Beat Saber's menu pointer (VRUIControls.
            // VRGraphicRaycaster) casts from the controller transform, so the
            // offset hands below put the ray 0.25 m under your gaze — you
            // must aim high by an amount you can only learn by feel, and with
            // no text on screen there is nothing to aim *at*. Collapsed, the
            // ray IS the gaze ray and the viewport centre is a crosshair.
            // Off by default: the offset is the honest emulation (a real
            // controller is not at your eye) and it is what puts the in-game
            // controller models where a body would hold them.
            static int aim_eye = -1;
            if (aim_eye < 0) aim_eye = kl_env_on("KL_VIEW_AIM_AT_EYE", 0);
            const float ox[2] = { -0.22f, 0.22f };
            for (int hand = 0; hand < 2; hand++) {
                // Rotate the head-relative offset (ox, -0.25, -0.40) by the
                // head quat: v' = q ⊗ v ⊗ q⁻¹ (unit q, expanded).
                float vx = ox[hand], vy = -0.25f, vz = -0.40f;
                float tx = 2.0f * (hqy * vz - hqz * vy);
                float ty = 2.0f * (hqz * vx - hqx * vz);
                float tz = 2.0f * (hqx * vy - hqy * vx);
                float hx = px + vx + hqw * tx + (hqy * tz - hqz * ty);
                float hy = py + vy + hqw * ty + (hqz * tx - hqx * tz);
                float hz = pz + vz + hqw * tz + (hqx * ty - hqy * tx);
                if (aim_eye) { hx = px; hy = py; hz = pz; }
                kl_ovrp_set_hand_pose(hand, hx, hy, hz, hqx, hqy, hqz, hqw);
            }

            // ControllerState4.Buttons carries OVRPlugin's RAW bits
            // (OVRInput.RawButton / ovrpButton), NOT the virtual OVRInput.Button
            // enum — the guest's metadata defines both (Button/Touch/Axis1D and
            // RawButton/RawTouch/RawAxis1D), and OVRInput maps raw->virtual
            // itself through its buttonMap. This block used to emit virtual
            // values, where 0x2000 is PrimaryIndexTrigger; in raw space 0x2000
            // is RThumbstickDown, so every "trigger press" was a stick flick and
            // no press ever reached a UI. Raw values below; KL_OVRP_FAKE_BUTTONS
            // is the check that the bits, not the pointer, were the problem.
            uint32_t rb = 0, lb = 0;   // right/left RAW button bits
            float ridx = mouse_l ? 1.0f : 0.0f, rgrip = mouse_r ? 1.0f : 0.0f;
            float lidx = keys[SDL_SCANCODE_G] ? 1.0f : 0.0f;
            float lgrip = keys[SDL_SCANCODE_H] ? 1.0f : 0.0f;
            float sx = 0, sy2 = 0;
            if (ridx > 0) rb |= 0x04000000;             // RIndexTrigger
            if (rgrip > 0) rb |= 0x08000000;            // RHandTrigger
            if (lidx > 0) lb |= 0x10000000;             // LIndexTrigger
            if (lgrip > 0) lb |= 0x20000000;            // LHandTrigger
            if (keys[SDL_SCANCODE_Z]) rb |= 0x00000001; // A
            if (keys[SDL_SCANCODE_X]) rb |= 0x00000002; // B
            if (keys[SDL_SCANCODE_C]) lb |= 0x00000100; // X
            if (keys[SDL_SCANCODE_V]) lb |= 0x00000200; // Y
            // Q = Menu (Start, 0x00100000), E = System (Back, 0x00200000) —
            // two DIFFERENT physical buttons on a Sense controller (Create /
            // Options, and the PlayStation button), so two keys. On the OpenXR
            // path Back is `/user/hand/right/input/system/click`, which is how
            // a SteamVR guest is asked for its dashboard; nothing set it until
            // SL-21, and it is driven from here as well so the path can be
            // exercised with no headset and no controller paired.
            if (keys[SDL_SCANCODE_Q]) { lb |= 0x00100000; rb |= 0x00100000; }
            if (keys[SDL_SCANCODE_E]) { lb |= 0x00200000; rb |= 0x00200000; }
            if (keys[SDL_SCANCODE_UP])    { rb |= 0x00001000; sy2 =  1.0f; }  // RThumbstickUp
            if (keys[SDL_SCANCODE_DOWN])  { rb |= 0x00002000; sy2 = -1.0f; }  // RThumbstickDown
            if (keys[SDL_SCANCODE_LEFT])  { rb |= 0x00004000; sx  = -1.0f; }  // RThumbstickLeft
            if (keys[SDL_SCANCODE_RIGHT]) { rb |= 0x00008000; sx  =  1.0f; }  // RThumbstickRight
            // Touches mirror buttons: a pressed button is a touched one.
            kl_ovrp_set_controller_input(1, rb, rb, ridx, rgrip, sx, sy2);
            kl_ovrp_set_controller_input(0, lb, lb, lidx, lgrip, 0.0f, 0.0f);
        }

        // ---- frame out.
        //
        // Hardware: one Metal pass, no pixels on this side at all.
        // kl_viewmtl_start is retried until it takes, because there is nothing
        // to composite until the guest has taken its eye textures — that
        // happens inside nativeRecreateGfxState, seconds into the run.
        if (hw) {
            if (!hw_up) {
                hw_up = kl_viewmtl_start(mlayer);
                if (!hw_up && !hw_warned && t_now - t_start > 15000) {
                    hw_warned = 1;
                    fprintf(stderr, "view: 15 s in and the guest has not asked "
                                    "for eye textures — nothing to composite "
                                    "yet. KL_VIEW_CPU=1 forces the readback "
                                    "path.\n");
                }
            }
            if (hw_up) {
                // Pixels, not points: a Retina window's drawable is 2x its
                // logical size, and a drawableSize in points renders the eye
                // into a quarter of the window and then upscales it.
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(win, &pw, &ph);
                kl_viewmtl_present(pw, ph);
            }
        } else {
            view_show_cpu_frame(&disp, ren, win);
        }

        // The HUD is one stderr line a second, not text rendering: a blank
        // frame with a moving pose and a nonzero lit count is still a live
        // pipeline, and that distinction is the whole point of the line.
        //
        // `frame` is the GUEST's frame count in both paths — swaps — because
        // that is the number worth watching and the number the two paths can
        // be compared on. The hardware path adds `shown`, which is display-
        // paced and will lag it: the guest is free to run faster than 60 Hz
        // now that nothing stalls it, and the compositor simply skips the
        // frames the display cannot show. On the readback path the two are the
        // same number by construction, since every swap IS a readback.
        //
        // `lit` on the hardware path is estimated from the 64x64 downsample
        // the compositor renders alongside the drawable — nothing is read back
        // for kl_glfb to count exactly.
        if (t_now - hud_last >= 1000) {
            hud_last = t_now;
            char frames[64];
            if (hw) snprintf(frames, sizeof frames, "frame %llu, shown %u",
                             kl_viewmtl_guest_frame(), kl_viewmtl_frames());
            else    snprintf(frames, sizeof frames, "frame %u", g_frame_seq);
            // A flat guest has no pose, and printing one would invite the
            // reader to debug a number that means nothing here. `lit` still
            // matters in both modes — it is what separates a blank frame from
            // a dead pipeline.
            if (mono)
                fprintf(stderr, "view: [mono] %s, lit=%lu\n",
                        frames, kl_glfb_last_frame_lit());
            else
                fprintf(stderr,
                    "view: %s, pose (%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f, lit=%lu\n",
                    frames, px, py, pz,
                    (double)(yaw * 180.0f / (float)M_PI),
                    (double)(pitch * 180.0f / (float)M_PI),
                    hw ? kl_viewmtl_lit() : kl_glfb_last_frame_lit());
        }

        // ~60 Hz: pace on the remainder rather than a blind SDL_Delay(16),
        // so a slow upload does not compound into slower frames.
        uint64_t elapsed = SDL_GetTicks() - t_now;
        if (elapsed < 16) SDL_Delay((uint32_t)(16 - elapsed));
    }

    unsigned shown = hw ? kl_viewmtl_frames() : g_frame_seq;
    kl_viewmtl_stop();          // unregisters the fence before the event goes
    view_cpu_disp_free(&disp);
    if (mview) SDL_Metal_DestroyView(mview);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    fprintf(stderr, "view: window closed, %u frames displayed\n", shown);
    return 0;
}

#else  // !KL_VIEW_HAVE_SDL

int kl_view_main(const char *libdir, int hw) {
    (void)libdir; (void)hw;
    fprintf(stderr, "view: KL_VIEW=1 but this binary was built without SDL3 "
                    "(pkg-config sdl3 not present at build time)\n");
    return 1;
}

#endif
