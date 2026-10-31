// How the game is running, a second at a time.
//
// What KL_STATS_CARD shows in the headset (KleptonMetrics.swift), sampled here
// and not on the compositor's thread: a sample walks every thread of the
// process, and a meter that makes the compositor late once a second would be
// measuring itself. The compositor only counts — a presented picture, whether
// it was a new one, the GPU span of its own pass — and the panel reads the
// newest sample.
//
// The frame rate is counted where a picture is SHOWN, not where a frame starts:
// only a guest that is paced through the XR API has a count of the frames it
// started (kl_app.c, guest_pace_wait), a Unity game has none, and what its
// player sees is this one anyway. A presented picture that showed the game's
// previous one again is a "repeated" picture — the compositor reprojects it to
// the newer head pose, so the world holds still, but what moves in the game
// does not move in it. A game that is deliberately drawn on every second
// refresh (KL_GUEST_PACE_DIV) repeats half its pictures by design, and the
// number says so.
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include "kl_metrics.h"
#include "kl_mtl_busy.h"

static _Atomic int g_on;
static _Atomic unsigned long long g_shown, g_fresh, g_stale, g_frames;
static _Atomic int g_eye_w, g_eye_h;
static struct { double gpu_s; unsigned long n; } g_comp;     // under g_mu
static kl_metrics g_now;                                     // under g_mu
static struct { int state, w, h; char path[40], note[24]; } g_up = { .state = -1 };  // under g_mu
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char now[24]; unsigned changes; } g_tag;     // under g_mu

void kl_metrics_begin(void) {
    if (atomic_exchange(&g_on, 1)) return;
    int gpu = kl_mtl_busy_install();
    fprintf(stderr, "[metrics] a sample a second for the panel in the headset; the GPU time of every "
                    "command buffer is %s\n", gpu ? "counted" : "NOT available");
}

void kl_metrics_note_shown(int fresh, int stale) {
    if (!g_on) return;
    g_shown++;
    if (fresh) g_fresh++;
    else if (stale) g_stale++;
}

void kl_metrics_note_composite(double gpu_seconds) {
    if (!g_on || !(gpu_seconds > 0)) return;
    pthread_mutex_lock(&g_mu);
    g_comp.gpu_s += gpu_seconds; g_comp.n++;
    pthread_mutex_unlock(&g_mu);
}

void kl_metrics_note_eye(int w, int h) { g_eye_w = w; g_eye_h = h; }

void kl_metrics_note_upscale(int state, int out_w, int out_h, const char *path, const char *note) {
    pthread_mutex_lock(&g_mu);
    g_up.state = state; g_up.w = out_w; g_up.h = out_h;
    snprintf(g_up.path, sizeof g_up.path, "%s", path ? path : "");
    snprintf(g_up.note, sizeof g_up.note, "%s", note ? note : "");
    pthread_mutex_unlock(&g_mu);
}
void kl_metrics_note_game_frame(void) { g_frames++; }

void kl_metrics_set_tag(const char *tag) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_tag.now, sizeof g_tag.now, "%s", tag ? tag : "");
    g_tag.changes++;
    pthread_mutex_unlock(&g_mu);
}

void kl_metrics_upscale_now(kl_metrics *out) {
    if (!out) return;
    pthread_mutex_lock(&g_mu);
    out->upscale = g_up.state; out->out_w = g_up.w; out->out_h = g_up.h;
    memcpy(out->path, g_up.path, sizeof out->path);
    memcpy(out->upscale_note, g_up.note, sizeof out->upscale_note);
    pthread_mutex_unlock(&g_mu);
}

unsigned kl_metrics_latest(kl_metrics *out) {
    pthread_mutex_lock(&g_mu);
    unsigned seq = g_now.seq;
    if (seq && out) *out = g_now;
    pthread_mutex_unlock(&g_mu);
    return seq;
}

// ---- showing and hiding the panel with the controllers ----------------------
//
// Each of the two buttons is in one of five states. A press waits (kPending)
// before it is passed on (kPassing), because whether it is the first half of
// "both together" is only known a moment later, and a game that has already
// been told "menu" has already opened it. A press that ends while it waits was
// a tap and is played afterwards (kTap). Both down is kChord for both, whatever
// they were, and stays that until both are up: letting one go must not turn the
// other into a press of its own.
enum { kIdle, kPending, kPassing, kTap, kChord };
#define KL_PILL_WAIT_S  0.25     // how long a press waits for the other button
#define KL_PILL_TAP_S   0.10     // how long a tap is played once it is known to be one
#define KL_PILL_CHORD_S 1.0      // how long both are held to switch the panel
static struct { int phase; double t; } g_pill[2];
static double g_chord_since;
static int g_chord_spent;                  // switched already, or one of the two was let go
static int g_chord_switched;               // this chord switched the panel
static _Atomic int g_visible;

void kl_metrics_set_visible(int visible) { g_visible = visible != 0; }
int kl_metrics_visible(void) { return g_visible; }

int kl_metrics_chord(int left_down, int right_down, double now, int *pass_left, int *pass_right) {
    int down[2] = { left_down != 0, right_down != 0 }, switched = 0;
    if (down[0] && down[1]) {
        if (g_pill[0].phase != kChord) {
            g_pill[0].phase = g_pill[1].phase = kChord;
            g_chord_since = now; g_chord_spent = 0; g_chord_switched = 0;
        } else if (!g_chord_spent && now - g_chord_since >= KL_PILL_CHORD_S) {
            g_chord_spent = g_chord_switched = 1;
            int visible = !g_visible;
            g_visible = visible;
            switched = 1;
            fprintf(stderr, "[metrics] both menu buttons held: the panel is %s\n", visible ? "shown" : "hidden");
        }
    } else if (g_pill[0].phase == kChord) {
        g_chord_spent = 1;                 // held together means without a gap
        if (!down[0] && !down[1]) {
            g_pill[0].phase = g_pill[1].phase = kIdle;
            // Both up again within the second, and nothing switched: a tap of the two together. Said
            // only while the panel is there to show what it did.
            if (!g_chord_switched && now - g_chord_since < KL_PILL_CHORD_S && g_visible) switched = 2;
        }
    }
    for (int h = 0; h < 2; h++) {
        switch (g_pill[h].phase) {
        case kIdle:    if (down[h]) { g_pill[h].phase = kPending; g_pill[h].t = now; } break;
        case kPending: if (!down[h]) { g_pill[h].phase = kTap; g_pill[h].t = now; }
                       else if (now - g_pill[h].t >= KL_PILL_WAIT_S) g_pill[h].phase = kPassing;
                       break;
        case kPassing: if (!down[h]) g_pill[h].phase = kIdle; break;
        case kTap:     if (now - g_pill[h].t >= KL_PILL_TAP_S) g_pill[h].phase = kIdle; break;
        default: break;
        }
    }
    if (pass_left)  *pass_left  = g_pill[0].phase == kPassing || g_pill[0].phase == kTap;
    if (pass_right) *pass_right = g_pill[1].phase == kPassing || g_pill[1].phase == kTap;
    return switched;
}

static double seconds(void) {
    struct timespec m;
    clock_gettime(CLOCK_MONOTONIC, &m);
    return (double)m.tv_sec + (double)m.tv_nsec * 1e-9;
}

// The CPU the process is using, from the scheduler's own running figure for
// each thread (an average that follows within about a second, which is the
// grain of the panel): the sum in cores, and the busiest thread with its name —
// a game that does not hold its rate while the GPU is half idle is waiting for
// that one.
static void sample_cpu(kl_metrics *m) {
    m->cpu_cores = m->cpu_top = -1; m->cpu_top_name[0] = 0;
    thread_act_array_t th = NULL;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    double sum = 0, top = -1;
    mach_msg_type_number_t top_at = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_basic_info_data_t bi;
        mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS) continue;
        if (bi.flags & TH_FLAGS_IDLE) continue;
        double u = (double)bi.cpu_usage / (double)TH_USAGE_SCALE;
        sum += u;
        if (u > top) { top = u; top_at = i; }
    }
    if (top >= 0) {
        m->cpu_cores = (float)sum; m->cpu_top = (float)(top > 1 ? 1 : top);
        pthread_t p = pthread_from_mach_thread_np(th[top_at]);
        if (p) pthread_getname_np(p, m->cpu_top_name, sizeof m->cpu_top_name);
    }
    for (mach_msg_type_number_t i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), th[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
}

static void sample_memory(kl_metrics *m) {
    m->mem_used_mb = m->mem_left_mb = -1;
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        m->mem_used_mb = (int)(info.phys_footprint >> 20);
#if TARGET_OS_IPHONE
    size_t left = os_proc_available_memory();
    if (left) m->mem_left_mb = (int)(left >> 20);
#endif
}

int kl_metrics_tick(void) {
    static double t_last;                                    // the heartbeat's thread only
    static unsigned long long shown_last, fresh_last, stale_last, frames_last;
    if (!g_on) return 0;
    double t = seconds();
    unsigned long long shown = g_shown, fresh = g_fresh, stale = g_stale, frames = g_frames;
    double dt = t - t_last;
    if (t_last && dt < 1.0) return 0;
    kl_metrics m = {0};
    double busy = kl_mtl_busy_take(NULL);
    unsigned long compiles = 0, cached = 0;
    kl_mtl_compiles_take(&compiles, &cached);
    pthread_mutex_lock(&g_mu);
    double comp_s = g_comp.gpu_s; unsigned long comp_n = g_comp.n;
    g_comp.gpu_s = 0; g_comp.n = 0;
    pthread_mutex_unlock(&g_mu);
    int first = !t_last;
    unsigned long long d_fresh = fresh - fresh_last, d_frames = frames - frames_last;
    t_last = t;
    m.fps = (float)((double)d_fresh / dt);
    m.display_hz = (float)((double)(shown - shown_last) / dt);
    m.stale = (float)((double)(stale - stale_last) / dt);
    m.game_fps = (float)((double)d_frames / dt);
    m.compiles = (float)((double)compiles / dt);
    m.compiles_cached = (float)((double)cached / dt);
    shown_last = shown; fresh_last = fresh; stale_last = stale; frames_last = frames;
    if (first) return 0;                                     // the first call only sets the marks
    sample_cpu(&m);
    sample_memory(&m);
    m.gpu_comp_ms = comp_n ? (float)(comp_s * 1000.0 / (double)comp_n) : -1;
    m.gpu_busy = m.gpu_game_ms = -1;
    if (busy >= 0) {
        m.gpu_busy = (float)(busy / dt);
        if (m.gpu_busy > 1) m.gpu_busy = 1;
        // Over the frames the game drew where that is counted: a guest that draws more often than it is
        // shown would otherwise be charged several frames' work for each picture.
        unsigned long long drew = d_frames > d_fresh ? d_frames : d_fresh;
        double game_s = busy - comp_s;
        if (drew) m.gpu_game_ms = (float)((game_s > 0 ? game_s : 0) * 1000.0 / (double)drew);
    }
    m.eye_w = g_eye_w; m.eye_h = g_eye_h;
    static int trace = -1;
    if (trace < 0) { const char *v = getenv("KL_PERF_LOG"); trace = v && atoi(v) != 0; }
    if (trace && g_now.seq % 5 == 0)
        fprintf(stderr, "  [perf] fresh=%.1f fps display=%.1f Hz stale=%.1f/s gpu=%.1f%% game=%.2fms composite=%.2fms eye=%dx%d\n",
                m.fps, m.display_hz, m.stale, m.gpu_busy*100, m.gpu_game_ms, m.gpu_comp_ms, m.eye_w, m.eye_h);
    pthread_mutex_lock(&g_mu);
    m.upscale = g_up.state; m.out_w = g_up.w; m.out_h = g_up.h;
    memcpy(m.path, g_up.path, sizeof m.path);
    memcpy(m.upscale_note, g_up.note, sizeof m.upscale_note);
    m.seq = g_now.seq + 1;
    g_now = m;
    static unsigned tag_changes;                             // the heartbeat's thread only
    char tag[24];
    snprintf(tag, sizeof tag, "%s", g_tag.changes != tag_changes ? "switch" : g_tag.now);
    tag_changes = g_tag.changes;
    pthread_mutex_unlock(&g_mu);
    // What the panel showed, into the log every fifth sample: the panel is in
    // the headset and a number read off it is otherwise lost with the session.
    // Every sample while a label is set (kl_metrics_set_tag).
    if (tag[0])
        fprintf(stderr, "[metrics %s] %.1f fps shown of %.1f Hz (%.1f repeated), %.1f started, game GPU %.1f ms "
                        "a frame, composite %.1f ms, GPU busy %.0f%%, CPU %.2f cores (top thread "
                        "%.0f%%), eye %dx%d\n", tag, (double)m.fps, (double)m.display_hz,
                (double)m.stale, (double)m.game_fps, (double)m.gpu_game_ms, (double)m.gpu_comp_ms,
                (double)m.gpu_busy * 100.0, (double)m.cpu_cores, (double)m.cpu_top * 100.0,
                m.eye_w, m.eye_h);
    else if (m.seq % 5 == 0)
        fprintf(stderr, "[metrics] %.1f fps shown of %.1f Hz (%.1f repeated), %.1f started, game GPU %.1f ms "
                        "a frame, composite %.1f ms, GPU busy %.0f%%, CPU %.2f cores (top thread "
                        "%.0f%%), eye %dx%d\n", (double)m.fps, (double)m.display_hz,
                (double)m.stale, (double)m.game_fps, (double)m.gpu_game_ms, (double)m.gpu_comp_ms,
                (double)m.gpu_busy * 100.0, (double)m.cpu_cores, (double)m.cpu_top * 100.0,
                m.eye_w, m.eye_h);
    return 1;
}
