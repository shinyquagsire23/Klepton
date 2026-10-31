// The stall watch: when the guest stops delivering frames, say what it was doing.
//
// "The game hangs for a moment now and then" has several quite different causes
// — a shader being compiled, a file being read on the wrong thread, memory being
// compressed because the process is at its limit, a lock — and they cannot be
// told apart from outside: each is a gap between two frames. So the frame path
// stamps every frame it submits (kl_stall_frame), and a watcher that sees no
// stamp for KL_STALL_MS milliseconds samples every thread of the process a few
// times while the gap lasts. When frames resume it prints how long the gap was
// and, for the threads that were not simply waiting, where they were: the leaf
// function and the chain above it, as image+offset with a symbol where there is
// one. A thread in Metal's compiler, in pread, or in the kernel's memory calls
// names itself.
//
// Nothing is suspended; a sample is whatever thread_get_state caught, and frame
// pointers are followed with vm_read_overwrite so a torn one costs a sample and
// not the process (the same care kl_sample.c takes).
//
//   KL_STALL_MS=<ms>   the gap that counts as a stall. 0: off. Default 0 here;
//                      a target's defaults may set it (kl_driver.c).
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kl_env.h"

#define KLS_DEPTH   14
#define KLS_SAMPLES 6
#define KLS_KEEP    48          // distinct (thread, stack) records kept per stall

static uint64_t g_last_ns;      // the last frame's stamp, CLOCK_UPTIME_RAW
static thread_t g_frame_thread; // who submits frames: shown even when it is waiting,
                                // because WHAT it waits in is often the answer
static uint64_t g_frames;
static int      g_threshold_ms = -1;

typedef struct { uintptr_t pc[KLS_DEPTH]; int n, hits; } kls_rec;

static uint64_t kls_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int kls_read(uintptr_t addr, void *out, size_t n) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &got)
           == KERN_SUCCESS && got == n;
}

// A leaf that only means "this thread has nothing to do".
static int kls_is_wait(uintptr_t pc) {
    Dl_info di;
    if (!dladdr((void *)pc, &di) || !di.dli_sname) return 0;
    static const char *w[] = { "__psynch_cvwait", "__psynch_mutexwait", "__ulock_wait", "__ulock_wait2",
        "mach_msg2_trap", "mach_msg_trap", "kevent", "kevent_id", "kevent64", "__semwait_signal",
        "semaphore_wait_trap", "semaphore_timedwait_trap", "__workq_kernreturn", "__select", "poll",
        "__sigsuspend", "__sigwait", "start_wqthread", "__accept", "__recvfrom", "read", "__read_nocancel",
        "nanosleep", "__nanosleep", "usleep", "os_sync_wait_on_address", "__os_sync_wait_on_address",
        // The guest's audio thread paces itself by a blocking write into a
        // small socket buffer (kl_lx_audio.c) and sits here most of its life.
        "write", "__write_nocancel" };
    for (unsigned i = 0; i < sizeof w / sizeof *w; i++)
        if (!strcmp(di.dli_sname, w[i])) {
            // read() is a wait only on a pipe or socket; on a file it is the
            // stall itself — and cannot be told apart here, so it is reported.
            return strcmp(w[i], "read") && strcmp(w[i], "__read_nocancel");
        }
    return 0;
}

static void kls_print_frame(uintptr_t pc) {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        base = base ? base + 1 : di.dli_fname;
        if (di.dli_sname)
            fprintf(stderr, "      %s  %.90s + %lu\n", base, di.dli_sname,
                    (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        else
            fprintf(stderr, "      %s + %#lx\n", base, (unsigned long)(pc - (uintptr_t)di.dli_fbase));
    } else {
        fprintf(stderr, "      %#lx\n", (unsigned long)pc);
    }
}

static void kls_sample(kls_rec *recs, int *nrecs, thread_t self) {
    thread_act_array_t th = NULL;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        if (th[i] != self) {
            arm_thread_state64_t st;
            mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
                kls_rec r;
                memset(&r, 0, sizeof r);
                r.pc[r.n++] = (uintptr_t)arm_thread_state64_get_pc(st);
                int waiting = kls_is_wait(r.pc[0]);
                {
                    uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
                    uintptr_t lr = (uintptr_t)arm_thread_state64_get_lr(st);
                    if (lr) r.pc[r.n++] = lr;
                    while (r.n < KLS_DEPTH && fp) {
                        uintptr_t pair[2];
                        if (!kls_read(fp, pair, sizeof pair) || pair[0] <= fp) break;
                        if (pair[1] && pair[1] != r.pc[r.n - 1]) r.pc[r.n++] = pair[1];
                        fp = pair[0];
                    }
                }
                // A waiting thread is worth showing when it is the frame thread,
                // or when what it waits IN is the graphics stack: a pipeline
                // being compiled is a thread asleep inside Metal, the work
                // itself being in another process.
                int keep = !waiting || th[i] == g_frame_thread;
                for (int f = 0; !keep && f < r.n; f++) {
                    Dl_info di;
                    if (dladdr((void *)r.pc[f], &di) && di.dli_fname &&
                        (strstr(di.dli_fname, "MoltenVK") || strstr(di.dli_fname, "/Metal.framework") ||
                         strstr(di.dli_fname, "IOGPU") || strstr(di.dli_fname, "AGX")))
                        keep = 1;
                    // MoltenVK is linked into the app on the device, so its frames
                    // carry the app's image name: go by the symbol there.
                    if (!keep && di.dli_sname && (strstr(di.dli_sname, "MVK") || strstr(di.dli_sname, "vkCreate")))
                        keep = 1;
                }
                if (keep) {
                    int k;
                    for (k = 0; k < *nrecs; k++)
                        if (recs[k].n == r.n && !memcmp(recs[k].pc, r.pc, (size_t)r.n * sizeof r.pc[0])) break;
                    if (k < *nrecs) recs[k].hits++;
                    else if (*nrecs < KLS_KEEP) { r.hits = 1; recs[(*nrecs)++] = r; }
                }
            }
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
}

static void *kls_main(void *arg) {
    (void)arg;
    pthread_setname_np("klepton.stall");
    thread_t self = mach_thread_self();
    static kls_rec recs[KLS_KEEP];
    unsigned long stalls = 0;
    uint64_t reported = 0;      // a gap already printed while it lasted
    for (;;) {
        usleep(20000);
        uint64_t last = __atomic_load_n(&g_last_ns, __ATOMIC_ACQUIRE);
        if (!last || last == reported || (kls_now() - last) / 1000000 < (uint64_t)g_threshold_ms) continue;
        // A gap. Sample while it lasts, a few times, further apart each time so
        // a long one is covered without being flooded.
        int nrecs = 0, taken = 0;
        unsigned wait_us = 30000;
        while (__atomic_load_n(&g_last_ns, __ATOMIC_ACQUIRE) == last) {
            if (taken < KLS_SAMPLES) { kls_sample(recs, &nrecs, self); taken++; }
            usleep(wait_us);
            if (wait_us < 400000) wait_us *= 2;
            // Not a stall: the game stopped. Said once, after 8 s rather than
            // 60: a player looking at a frozen picture quits long before a
            // minute, and the report never came (Battle Sister, 2026-10-28).
            if ((kls_now() - last) / 1000000 > 8000) { reported = last; break; }
        }
        // The gap is from the last frame to the NEXT one, not to whenever this
        // thread next looked: measured to "now" every stall came out as one of
        // 480, 720, 1200, 1680 ms — this loop's own sleeps.
        uint64_t next = __atomic_load_n(&g_last_ns, __ATOMIC_ACQUIRE);
        uint64_t gap_ms = ((next != last ? next : kls_now()) - last) / 1000000;
        // Keep the log readable in a session that stalls often: every stall is
        // counted, the long ones and the first dozens are spelled out.
        stalls++;
        if (stalls > 60 && gap_ms < 400) continue;
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(stderr, "  [stall] %02d:%02d:%02d  no frame for %llu ms (stall %lu, after frame %llu); "
                        "%d sample(s); the frame thread and the threads that were not waiting:\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                (unsigned long long)gap_ms, stalls,
                (unsigned long long)__atomic_load_n(&g_frames, __ATOMIC_RELAXED), taken);
        // Most-sampled first; three stacks say what a stall was.
        for (int shown = 0; shown < 3; shown++) {
            int best = -1;
            for (int k = 0; k < nrecs; k++)
                if (recs[k].hits > 0 && (best < 0 || recs[k].hits > recs[best].hits)) best = k;
            if (best < 0) break;
            fprintf(stderr, "    in %d of %d sample(s):\n", recs[best].hits, taken);
            for (int f = 0; f < recs[best].n; f++) kls_print_frame(recs[best].pc[f]);
            recs[best].hits = 0;
        }
        if (!nrecs) fprintf(stderr, "    (every thread was waiting)\n");
    }
    return NULL;
}

// Called by the frame path each time the guest hands over a frame.
void kl_stall_frame(void) {
    if (g_threshold_ms < 0) {
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&mu);
        if (g_threshold_ms < 0) {
            int ms = kl_env_int("KL_STALL_MS", 0);
            if (ms > 0) {
                pthread_t th;
                pthread_attr_t at;
                pthread_attr_init(&at);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&th, &at, kls_main, NULL) != 0) ms = 0;
                else fprintf(stderr, "  [stall] watching for gaps of %d ms or more between frames\n", ms);
            }
            g_threshold_ms = ms > 0 ? ms : 0;
        }
        pthread_mutex_unlock(&mu);
    }
    if (!g_threshold_ms) return;
    if (!g_frame_thread) g_frame_thread = pthread_mach_thread_np(pthread_self());
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_ns, kls_now(), __ATOMIC_RELEASE);
}
