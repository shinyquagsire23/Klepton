// See kl_present.h for what this is and why the mode is observed rather than
// configured. The implementation is deliberately tiny: two facts in, one
// derived answer out, and a generation counter so a consumer can act on a
// transition exactly once.
#include <stdio.h>
#include "kl_present.h"

static int      g_have_window;
static int      g_win_w, g_win_h;
static int      g_have_eyes;
static unsigned g_generation;
static kl_present_mode g_mode = KL_PRESENT_NONE;

// The whole policy, in one place. Stereo beats mono because it is the more
// specific fact: every Android guest creates a window surface, including Unity,
// so "has a window" cannot distinguish the two on its own — but "has eye
// textures" can.
static kl_present_mode derive(void) {
    if (g_have_eyes)   return KL_PRESENT_STEREO;
    if (g_have_window) return KL_PRESENT_MONO;
    return KL_PRESENT_NONE;
}

static void settle(void) {
    kl_present_mode m = derive();
    if (m == g_mode) return;
    static const char *const NAME[] = { "none", "mono (window)", "stereo (immersive)" };
    fprintf(stderr, "  [present] %s -> %s (generation %u)\n",
            NAME[g_mode], NAME[m], g_generation + 1);
    g_mode = m;
    g_generation++;
}

kl_present_mode kl_present_mode_now(void) { return g_mode; }
unsigned        kl_present_generation(void) { return g_generation; }

void kl_present_mono_size(int *w, int *h) {
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
}

void kl_present_note_window_surface(int w, int h) {
    if (w > 0 && h > 0) { g_win_w = w; g_win_h = h; }
    g_have_window = 1;
    settle();
}

void kl_present_note_eye_texture(void) {
    g_have_eyes = 1;
    settle();
}

void kl_present_clear_eye_textures(void) {
    g_have_eyes = 0;
    settle();
}
