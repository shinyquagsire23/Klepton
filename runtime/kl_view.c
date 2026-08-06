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
//   frame out  — kl_glfb hands each swap's readback to kl_view_frame_sink
//                (registered by t_boot), which stores it under a mutex; this
//                loop uploads the newest frame to a streaming texture. On
//                visionOS the same seam is where Compositor Services attaches.
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
#include "kl_ovrp.h"     // kl_ovrp_set_head_pose — the pose-in seam

#if __has_include(<SDL3/SDL.h>)
#define KL_VIEW_HAVE_SDL 1
#include <SDL3/SDL.h>
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

int kl_view_main(const char *libdir) {
    (void)libdir;   // the title names the frontend, not the target
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "view: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Klepton — Beat Saber, one eye",
                                       KL_VIEW_WIN_W, KL_VIEW_WIN_H, 0);
    if (!win) {
        fprintf(stderr, "view: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        fprintf(stderr, "view: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // Head state. Position starts at standing eye height; yaw/pitch start at
    // zero, facing −z per the convention block above.
    float px = 0, py = KL_VIEW_EYE_HEIGHT, pz = 0;
    float yaw = 0, pitch = 0;
    int mouselook = 0;

    SDL_Texture *tex = NULL;       // created on the first frame, size unknown till then
    int tex_w = 0, tex_h = 0;
    uint8_t *flip = NULL;          // top-down staging for SDL_UpdateTexture
    size_t flip_cap = 0;

    int done = 0;
    uint64_t t_prev = SDL_GetTicks();
    uint64_t hud_last = t_prev;
    while (!done) {
        uint64_t t_now = SDL_GetTicks();
        float dt = (float)(t_now - t_prev) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;  // a stall is not a teleport
        t_prev = t_now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
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
        {
            float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
            float syw = sinf(yaw * 0.5f), cyw = cosf(yaw * 0.5f);
            // q = yaw ⊗ pitch: quat multiply of (0,syw,0,cyw) and (sp,0,0,cp).
            kl_ovrp_set_head_pose(px, py, pz,
                                  cyw * sp, cp * syw, -syw * sp, cyw * cp);
        }

        // Newest frame, if the sink stored one since last time: flip GL's
        // bottom-up rows into top-down and upload. SDL_PIXELFORMAT_RGBA32 is
        // the byte-order alias whose memory layout is R,G,B,A — exactly what
        // the glReadPixels(GL_RGBA) in kl_glfb produced.
        pthread_mutex_lock(&g_frame_mu);
        int have = g_frame_new, w = g_frame_w, h = g_frame_h;
        if (have) {
            size_t n = (size_t)w * (size_t)h * 4;
            if (n > flip_cap) {
                uint8_t *nb = realloc(flip, n);
                if (nb) { flip = nb; flip_cap = n; }
            }
            if (flip) {
                size_t stride = (size_t)w * 4;
                for (int y = 0; y < h; y++)
                    memcpy(flip + stride * (size_t)y,
                           g_frame_buf + stride * (size_t)(h - 1 - y), stride);
            } else {
                have = 0;
            }
            g_frame_new = 0;
        }
        pthread_mutex_unlock(&g_frame_mu);

        if (have) {
            if (!tex || w != tex_w || h != tex_h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
                tex_w = w;
                tex_h = h;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, flip, w * 4);
        }

        if (tex) {
            // Letterbox: the eye is 1832x1920-ish, the window is not.
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            float scale = fminf((float)ww / tex_w, (float)wh / tex_h);
            SDL_FRect dst = {
                (ww - tex_w * scale) * 0.5f, (wh - tex_h * scale) * 0.5f,
                tex_w * scale, tex_h * scale,
            };
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, &dst);
            SDL_RenderPresent(ren);
        }

        // The HUD is one stderr line a second, not text rendering: a blank
        // frame with a moving pose and a nonzero lit count is still a live
        // pipeline, and that distinction is the whole point of the line.
        if (t_now - hud_last >= 1000) {
            hud_last = t_now;
            fprintf(stderr,
                    "view: frame %u, pose (%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f, lit=%lu\n",
                    g_frame_seq, px, py, pz,
                    (double)(yaw * 180.0f / (float)M_PI),
                    (double)(pitch * 180.0f / (float)M_PI),
                    kl_glfb_last_frame_lit());
        }

        // ~60 Hz: pace on the remainder rather than a blind SDL_Delay(16),
        // so a slow upload does not compound into slower frames.
        uint64_t elapsed = SDL_GetTicks() - t_now;
        if (elapsed < 16) SDL_Delay((uint32_t)(16 - elapsed));
    }

    free(flip);
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    fprintf(stderr, "view: window closed, %u frames displayed\n", g_frame_seq);
    return 0;
}

#else  // !KL_VIEW_HAVE_SDL

int kl_view_main(const char *libdir) {
    (void)libdir;
    fprintf(stderr, "view: KL_VIEW=1 but this binary was built without SDL3 "
                    "(pkg-config sdl3 not present at build time)\n");
    return 1;
}

#endif
