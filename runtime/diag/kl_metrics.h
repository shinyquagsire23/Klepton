// How the game is running, a second at a time — the numbers behind the panel
// in the headset (KL_STATS_CARD, visionos/Sources/KleptonMetrics.swift).
// See kl_metrics.c.
#ifndef KL_METRICS_H
#define KL_METRICS_H

// One second. Rates are per second; a value that is not known is negative
// (0 for the eye size).
typedef struct kl_metrics {
    unsigned seq;              // counts the samples taken; 0 is "none yet"
    float fps;                 // new pictures of the game that were shown
    float display_hz;          // pictures the compositor presented — the rate the app is shown at
    float stale;               // of those, the ones that showed the game's previous picture again
    float game_fps;            // frames the game started, where that is counted (0 otherwise)
    float gpu_busy;            // share of the time the GPU worked for this app, 0..1
    float gpu_game_ms;         // GPU time of one game frame, the compositor's pass left out
    float gpu_comp_ms;         // GPU time of one composite pass
    float compiles;            // shaders and pipeline states Metal had to compile
    float compiles_cached;     // ...and the ones it had compiled before and only fetched
    float cpu_cores;           // CPU time the process used, in cores (2.0 = two cores' worth)
    float cpu_top;             // the busiest thread's share of one core, 0..1
    char  cpu_top_name[32];    // ...and its name, "" where it has none
    int   mem_used_mb;         // the app's footprint
    int   mem_left_mb;         // what is left before the system's limit (visionOS)
    int   eye_w, eye_h;        // the picture the game draws (render size), in pixels
    int   upscale;             // MetalFX: 0 off, 1 on but linear, 2 upscaled; -1 not said yet
    int   out_w, out_h;        // ...the size after the upscale, in the picture's pixels (0 when none)
    char  path[40];            // how the picture comes: "Vulkan · per eye", "OpenXR GL · 2 layers", ...
    char  upscale_note[24];    // ...and why it is linear, in a few words ("100 %", "budget")
} kl_metrics;

// Starts the counting. Nothing below counts before it, and kl_metrics_tick
// does nothing — a game whose panel is never shown pays for none of this.
void kl_metrics_begin(void);
// The compositor's side, one call per presented picture: whether it showed a
// new picture of the game or the previous one again (neither while the game
// has shown nothing)...
void kl_metrics_note_shown(int fresh, int stale);
// ...the GPU span of one composite command buffer, in seconds...
void kl_metrics_note_composite(double gpu_seconds);
// ...and the size of the eye picture it reads.
void kl_metrics_note_eye(int w, int h);
// ...and its MetalFX upscale (kl_metrics.upscale and what follows it). Strings
// are copied; NULL is "".
void kl_metrics_note_upscale(int state, int out_w, int out_h, const char *path, const char *note);
// The game's side, where its frames are counted: one call as a frame starts.
void kl_metrics_note_game_frame(void);
// Takes a sample when a second has passed since the last one; returns 1 when
// it did. Called a few times a second from a thread that may take a moment —
// the app's heartbeat — and never from the compositor's.
int kl_metrics_tick(void);
// A label for the samples from now on (NULL or "" = none): while one is set,
// EVERY sample goes to the log, tagged with it — for an A/B inside one session
// (KL_VK_FRAME_ASYNC_AB). A sample whose second saw the label change is tagged
// "switch". Copied.
void kl_metrics_set_tag(const char *tag);
// The newest sample; returns its seq (0 and an untouched *out before the first).
unsigned kl_metrics_latest(kl_metrics *out);
// The upscale as the compositor said it last (kl_metrics_note_upscale), into
// those fields of *out only — newer than the last sample's, so a switch (a tap
// of Create + Options) can be drawn at once rather than with the next second.
void kl_metrics_upscale_now(kl_metrics *out);

// ---- showing and hiding the panel with the controllers ---------------------
//
// The two menu buttons of the controllers — Create on the left Sense
// controller, Options on the right — held together for a second show the
// panel, and hide it again: in every game, whether or not its setting is on,
// which only says whether the panel is there from the start
// (kl_metrics_set_visible). Whether each of the two is down goes in,
// once per input poll, with the time in seconds; what comes out is whether each
// press is to reach the game. A press is held back for a quarter of a second to
// see whether the other one follows (a shorter tap is played afterwards), and
// while both are down neither reaches the game, so the game's own menu does not
// open with the panel. Returns 1 on the poll that switched the panel, and 2 on
// the poll that ended a TAP of the two together (both up again within the
// second, nothing switched) while the panel was shown — which the panel itself does
// nothing with: the app switches MetalFX upscaling on it where MetalFX can
// upscale (KleptonMetalFX.tapToggle). 0 otherwise. One caller, one thread.
int kl_metrics_chord(int left_down, int right_down, double now_s, int *pass_left, int *pass_right);
// Whether the panel is to be drawn. It is not until someone says so: the
// game's setting at its start, or the two buttons.
void kl_metrics_set_visible(int visible);
int kl_metrics_visible(void);

#endif
