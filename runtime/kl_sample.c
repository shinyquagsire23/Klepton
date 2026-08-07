// Guest-thread sampler: where is the guest actually spending its time?
//
// Built for the loading-pace investigation (PLANNING.md "Loading-pace arc"):
// the game never finishes loading, the CPU is idle, and every wait primitive
// was exonerated — so the question became which managed code, if any, runs at
// all. KL_TRACE_IO/FUTEX/SLEEP measure shim calls; this measures the guest
// itself.
//
// Every KL_SAMPLE_MS milliseconds a sampler thread walks task_threads(),
// reads each thread's pc/fp with thread_get_state, and follows the x29 frame
// chain (AAPCS64: [fp] = {prev fp, lr}, as in t_boot's fault reporter). A
// thread counts as a guest thread when any frame lands in a loaded guest
// image (kl_addr_image). Each sample contributes two labels: the leaf frame
// (where the thread is — including host symbols like __psynch_cvwait, which
// is exactly what a blocked thread looks like) and the first guest frame
// (which guest code reached it). libil2cpp pcs resolve to managed method
// names through kl_il2cpp.c.
//
// Nothing here suspends the sampled threads: a sample is whatever the thread
// was doing when thread_get_state caught it, which over enough samples is the
// distribution we want. Stack reads go through vm_read_overwrite so a
// torn frame pointer costs one sample, not the process.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include "klepton.h"
#include "kl_il2cpp.h"
#include "kl_sample.h"

#define KL_SAMPLE_DEPTH 16          // frames walked per thread per sample
#define KL_MAX_THREADS  128

// One distinct label's hit count. Linear-scan arrays: the hot set of a
// stuck game is small, and this file is a diagnostic, not a fast path.
typedef struct { const char *label; uint64_t n; } count_rec;
typedef struct {
    count_rec *v;
    unsigned   n, cap;
} counts;

static void count_add(counts *c, const char *label) {
    for (unsigned i = 0; i < c->n; i++)
        if (c->v[i].label == label || strcmp(c->v[i].label, label) == 0) {
            c->v[i].n++;
            return;
        }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = realloc(c->v, c->cap * sizeof *c->v);
    }
    c->v[c->n].label = strdup(label);
    c->v[c->n].n = 1;
    c->n++;
}

static int cmp_counts(const void *pa, const void *pb) {
    uint64_t a = ((const count_rec *)pa)->n, b = ((const count_rec *)pb)->n;
    return a > b ? -1 : a < b;
}

typedef struct {
    mach_port_t act;              // thread port; identifies the thread
    char        name[64];
    uint64_t    samples;
    uint64_t    running, waiting, other_state;
    unsigned    stacks_printed;
    counts      leaf, guest, managed;
} thread_rec;

static pthread_t       g_thread;
static volatile int    g_stop;
static int             g_started;
static unsigned        g_interval_ms = 10;
static uint64_t        g_nsamples;
static counts          g_leaf, g_guest, g_managed;
static thread_rec      g_threads[KL_MAX_THREADS];
static unsigned        g_nthreads;
static mach_port_t     g_main_act;    // the thread that started the pump

// Parked-thread chain dumps (see sample_once). KL_SAMPLE_STACKS=0 opts out.
#define KL_SAMPLE_STACKS_MAX 3
static int t_stack_trace_enabled(void) {
    const char *e = getenv("KL_SAMPLE_STACKS");
    return !(e && e[0] == '0');
}

// Label one pc: managed method name, guest module+offset, host symbol, or
// a bare address. `buf` is used only when nothing interned matches.
static const char *label_pc(const void *pc, char *buf, size_t cap) {
    const char *m = kl_il2cpp_method_at(pc);
    if (m) return m;
    size_t off = 0;
    const char *img = kl_addr_image(pc, &off);
    if (img) {
        snprintf(buf, cap, "%s+0x%zx", img, off);
        return buf;
    }
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname) {
        snprintf(buf, cap, "%s+0x%tx", di.dli_sname,
                 (const char *)pc - (const char *)di.dli_saddr);
        return buf;
    }
    snprintf(buf, cap, "%p", pc);
    return buf;
}

static thread_rec *thread_rec_for(mach_port_t act) {
    for (unsigned i = 0; i < g_nthreads; i++)
        if (g_threads[i].act == act) return &g_threads[i];
    if (g_nthreads == KL_MAX_THREADS) return NULL;
    thread_rec *t = &g_threads[g_nthreads++];
    memset(t, 0, sizeof *t);
    t->act = act;
    if (act == g_main_act)
        snprintf(t->name, sizeof t->name, "main (pump)");
    pthread_t pt = pthread_from_mach_thread_np(act);
    if (pt && !t->name[0]) {
        char nm[64] = {0};
        if (pthread_getname_np(pt, nm, sizeof nm) == 0 && nm[0])
            snprintf(t->name, sizeof t->name, "%s", nm);
    }
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "thread %#x", act);
    return t;
}

static void sample_once(void) {
    thread_act_array_t list;
    mach_msg_type_number_t nlist = 0;
    if (task_threads(mach_task_self(), &list, &nlist) != KERN_SUCCESS) return;
    mach_port_t self = mach_thread_self();
    g_nsamples++;

    for (mach_msg_type_number_t i = 0; i < nlist; i++) {
        thread_act_t act = list[i];
        if (act == self) continue;

        arm_thread_state64_t ts;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(act, ARM_THREAD_STATE64,
                             (thread_state_t)&ts, &cnt) != KERN_SUCCESS)
            continue;

        // Walk the frame chain. frames[0] is the live pc; each further entry
        // is the lr of the frame above. The chain must rise monotonically —
        // anything else is a torn read of a running thread's stack.
        // fps[f] is the x29 of the frame frames[f] returns into (fps[0] is
        // the live fp); needed by the Class::Init waiter probe below, which
        // reads callee-saved registers out of a specific frame.
        void *frames[KL_SAMPLE_DEPTH];
        uint64_t fps[KL_SAMPLE_DEPTH];
        unsigned nf = 0;
        frames[nf] = (void *)ts.__pc;
        fps[nf++] = ts.__fp;
        uint64_t fp = ts.__fp;
        while (fp && nf < KL_SAMPLE_DEPTH) {
            if (fp & 7) break;
            uint64_t pair[2];
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), fp, sizeof pair,
                                  (vm_address_t)pair,
                                  &got) != KERN_SUCCESS || got != sizeof pair)
                break;
            if (!pair[1] || pair[0] <= fp) break;
            frames[nf] = (void *)pair[1];
            fps[nf++] = pair[0];
            fp = pair[0];
        }

        // A guest thread is one whose stack touches a guest image at all.
        // first_managed is the lowest frame that names a managed method — for
        // a thread parked in a wait, that is who is waiting.
        unsigned first_guest = 0;
        int is_guest = 0;
        const char *managed = NULL;
        for (unsigned f = 0; f < nf; f++) {
            if (kl_addr_image(frames[f], NULL)) {
                if (!is_guest) first_guest = f;
                is_guest = 1;
            }
            if (!managed) managed = kl_il2cpp_method_at(frames[f]);
        }
        if (!is_guest) continue;

        // One-shot class probe. Each entry is a generated-code guard slot
        // (vaddrs are Beat Saber 1.28's): the method's prologue guards a
        // klass via **slot before touching its statics. Print the class
        // identity and its init-state bytes — names what a Class::Init
        // waiter blocks on, or what a throwing cctor was initialising.
        // Slots: Dns::GetHostByName 0x1abc910, Dns::GetHostEntry 0x1abc72c,
        // <Initialize>d__6::MoveNext's failing guard at 0x15bdc7c.
        {
            static int slots_probed;
            if (!slots_probed && managed) {
                kl_image *il = kl_find_image("libil2cpp.so");
                const uint8_t *base = il ? kl_base(il) : NULL;
                static const struct { const char *site; uint64_t slot; }
                    probes[] = {
                        {"Dns::GetHostByName",      0x3ad6b98},
                        {"Dns::GetHostEntry",       0x3b1fb40},
                        {"<Initialize>d__6 guard",  0x3b7bfb0},
                    };
                unsigned resolved = 0;
                for (unsigned i = 0; base && i < 3; i++) {
                    uint64_t slotp = *(uint64_t *)(base + probes[i].slot);
                    uint64_t klass = slotp ? *(uint64_t *)slotp : 0;
                    if (!klass) continue;
                    resolved++;
                    const char *name = *(char **)(klass + 0x10);
                    const char *ns   = *(char **)(klass + 0x18);
                    fprintf(stderr, "  [sample] %s guards class %s.%s "
                            "(klass %#llx): init byte +0x12f = %#x, "
                            "word +0xe0 = %#x\n",
                            probes[i].site, ns ? ns : "?", name ? name : "?",
                            (unsigned long long)klass,
                            *(uint8_t *)(klass + 0x12f),
                            *(uint32_t *)(klass + 0xe0));
                }
                // The slots are populated during il2cpp's metadata init; a
                // sample before that finds zeros — retry next time.
                if (resolved) slots_probed = 1;
                // The <Initialize>d__6 abort (2026-08-06 night): MoveNext
                // *rethrows* an exception it built at libil2cpp+0x2246048
                // through the raise thunk 0x12e8c94. Name the two managed
                // helpers on that path — the exception's factory says what
                // is actually failing.
                if (base) {
                    const char *f = kl_il2cpp_method_at(base + 0x2246048);
                    const char *g = kl_il2cpp_method_at(base + 0x1db2cc0);
                    fprintf(stderr, "  [sample] Initialize abort path: "
                            "factory = %s; called-before = %s\n",
                            f ? f : "?", g ? g : "?");
                }
            }
        }

        // Class::Init waiter autopsy. A thread parked in libil2cpp's class
        // initializer has a frame returning to base+0x129c2c0 (the
        // instruction after `bl ConditionVariable::Wait` in Class::Init,
        // Beat Saber 1.28 layout). The frame below it (f-1) is
        // ConditionVariableImpl::Wait (0x1273630), whose prologue saved the
        // caller's x20/x19 at [x29-0x10]/[x29-0x8]. At the wait call site
        // Class::Init's live x20 is klass+0xe0 (x19 is already clobbered to
        // the constant 1 by the wait loop), so klass = saved_x20 - 0xe0.
        // That names *which* class each waiter blocks on — the DNS probe
        // above only knows the two guard slots baked into Dns, and
        // GetHostAddresses guards something else. Also print klass+0xdc
        // (init-in-progress flag) and klass+0xe8 (the initializing thread's
        // pthread_t): state==1 with waiters still asleep is a lost wake;
        // owner!=0 names the thread that never came back.
        {
            kl_image *il = kl_find_image("libil2cpp.so");
            const uint8_t *base = il ? kl_base(il) : NULL;
            if (base) {
                for (unsigned f = 1; f < nf; f++) {
                    if (frames[f] != base + 0x129c2c0) continue;
                    uint64_t saved[2];
                    vm_size_t got = 0;
                    if (vm_read_overwrite(mach_task_self(),
                                          fps[f - 1] - 0x10, sizeof saved,
                                          (vm_address_t)saved,
                                          &got) != KERN_SUCCESS ||
                        got != sizeof saved)
                        break;
                    uint64_t klass = saved[0] - 0xe0;    // live x20 - 0xe0
                    // Validity: the klass lives in the guest's runtime arena
                    // (not a loaded image), so accept it when its name and
                    // namespace pointers both read back as printable ASCII.
                    if (!klass)
                        break;
                    // The waiter's computed abstime sits at [sp] of the
                    // Wait frame (x29 = sp+0x60, so [x29-0x60]). Track it per
                    // (thread, klass) across samples: a ts that never moves
                    // means the thread is inside ONE wait that never returns
                    // (far-future or infinite); a moving ts means the 1ms
                    // poll loop is alive and something else is wrong.
                    struct { int64_t sec, nsec; } wts = {0, 0};
                    vm_read_overwrite(mach_task_self(),
                                      fps[f - 1] - 0x60, sizeof wts,
                                      (vm_address_t)&wts,
                                      &(vm_size_t){0});
                    static struct {
                        mach_port_t act; uint64_t klass;
                        int64_t sec, nsec;
                        unsigned frozen, changed, reported;
                    } trk[64];
                    static unsigned ntrk;
                    unsigned ti;
                    for (ti = 0; ti < ntrk; ti++)
                        if (trk[ti].act == act && trk[ti].klass == klass) break;
                    if (ti == ntrk) {
                        if (ntrk == 64) break;
                        ti = ntrk++;
                        memset(&trk[ti], 0, sizeof trk[ti]);
                        trk[ti].act = act; trk[ti].klass = klass;
                        trk[ti].sec = wts.sec; trk[ti].nsec = wts.nsec;
                        trk[ti].reported = 1;
                    } else if (trk[ti].sec != wts.sec || trk[ti].nsec != wts.nsec) {
                        trk[ti].changed++;
                        if (trk[ti].reported >= 1 && trk[ti].reported <= 8) {
                            struct timespec now;
                            clock_gettime(CLOCK_REALTIME, &now);
                            double old_d =
                                (double)(trk[ti].sec - now.tv_sec) +
                                (double)(trk[ti].nsec - now.tv_nsec) / 1e9;
                            double new_d =
                                (double)(wts.sec - now.tv_sec) +
                                (double)(wts.nsec - now.tv_nsec) / 1e9;
                            fprintf(stderr, "  [sample] Class::Init waiter "
                                    "(klass %#llx) ts moved #%u: delta "
                                    "%.3fs -> %.3fs\n",
                                    (unsigned long long)klass,
                                    trk[ti].changed, old_d, new_d);
                            trk[ti].reported++;
                        }
                        trk[ti].sec = wts.sec; trk[ti].nsec = wts.nsec;
                    } else {
                        trk[ti].frozen++;
                        if (trk[ti].reported <= 1 && trk[ti].frozen == 200) {
                            struct timespec now;
                            clock_gettime(CLOCK_REALTIME, &now);
                            fprintf(stderr, "  [sample] Class::Init waiter "
                                    "(klass %#llx) ts FROZEN across %u "
                                    "samples: one wait that never returns; "
                                    "ts %lld.%09ld (now %lld.%09ld, "
                                    "delta %.3fs)\n",
                                    (unsigned long long)klass, trk[ti].frozen,
                                    (long long)wts.sec, (long)wts.nsec,
                                    (long long)now.tv_sec, now.tv_nsec,
                                    (double)(wts.sec - now.tv_sec) +
                                    (double)(wts.nsec - now.tv_nsec) / 1e9);
                            trk[ti].reported = 2;
                        }
                    }
                    static uint64_t seen[64];
                    static unsigned nseen;
                    unsigned s;
                    for (s = 0; s < nseen; s++)
                        if (seen[s] == klass) break;
                    if (s == nseen) {
                        uint32_t state, inprog;
                        uint64_t owner;
                        const char *name, *ns;
                        if (vm_read_overwrite(mach_task_self(),
                                              klass + 0xdc, 4,
                                              (vm_address_t)&inprog,
                                              &(vm_size_t){0}) != KERN_SUCCESS)
                            break;
                        vm_read_overwrite(mach_task_self(), klass + 0xe0, 4,
                                          (vm_address_t)&state,
                                          &(vm_size_t){0});
                        vm_read_overwrite(mach_task_self(), klass + 0xe8, 8,
                                          (vm_address_t)&owner,
                                          &(vm_size_t){0});
                        vm_read_overwrite(mach_task_self(), klass + 0x10, 8,
                                          (vm_address_t)&name,
                                          &(vm_size_t){0});
                        vm_read_overwrite(mach_task_self(), klass + 0x18, 8,
                                          (vm_address_t)&ns,
                                          &(vm_size_t){0});
                        char nbuf[96], nsbuf[96];
                        vm_size_t g1 = 0, g2 = 0;
                        memset(nbuf, 0, sizeof nbuf); memset(nsbuf, 0, sizeof nsbuf);
                        vm_read_overwrite(mach_task_self(),
                                          (vm_address_t)name, sizeof nbuf - 1,
                                          (vm_address_t)nbuf, &g1);
                        vm_read_overwrite(mach_task_self(),
                                          (vm_address_t)ns, sizeof nsbuf - 1,
                                          (vm_address_t)nsbuf, &g2);
                        if (!g1 || nbuf[0] < 0x20 || nbuf[0] > 0x7e)
                            break;              // torn read, not a klass
                        if (nseen < 64) seen[nseen++] = klass;
                        struct timespec now;
                        clock_gettime(CLOCK_REALTIME, &now);
                        fprintf(stderr, "  [sample] Class::Init waiter on %s.%s "
                                "(klass %#llx): in-progress +0xdc = %#x, "
                                "state +0xe0 = %#x, owner +0xe8 = %#llx; "
                                "wait ts %lld.%09ld (now %lld.%09ld, "
                                "delta %.3fs)\n",
                                g2 ? nsbuf : "?", g1 ? nbuf : "?",
                                (unsigned long long)klass, inprog, state,
                                (unsigned long long)owner,
                                (long long)wts.sec, (long)wts.nsec,
                                (long long)now.tv_sec, now.tv_nsec,
                                (double)(wts.sec - now.tv_sec) +
                                (double)(wts.nsec - now.tv_nsec) / 1e9);
                    }
                    break;
                }
            }
        }

        // A thread parked in the same managed method across many samples is a
        // stuck wait, and "who called it" is the question that matters. Print
        // the full managed chain the first few times a thread is seen parked
        // in such a method (KL_SAMPLE_STACKS=0 disables).
        if (managed && t_stack_trace_enabled()) {
            thread_rec *tr = thread_rec_for(act);
            if (tr && tr->stacks_printed < KL_SAMPLE_STACKS_MAX &&
                tr->samples > 50 && tr->managed.n == 1) {
                tr->stacks_printed++;
                fprintf(stderr, "  [sample] thread %s parked in %s; chain:\n",
                        tr->name, managed);
                for (unsigned f = 0; f < nf; f++) {
                    char b[160];
                    fprintf(stderr, "    #%-2d %s\n", f,
                            label_pc(frames[f], b, sizeof b));
                }
            }
        }

        char lbuf[160], gbuf[160];
        const char *leaf  = label_pc(frames[0], lbuf, sizeof lbuf);
        const char *guest = label_pc(frames[first_guest], gbuf, sizeof gbuf);
        count_add(&g_leaf, leaf);
        count_add(&g_guest, guest);
        if (managed) count_add(&g_managed, managed);

        thread_rec *t = thread_rec_for(act);
        if (t) {
            t->samples++;
            count_add(&t->leaf, leaf);
            count_add(&t->guest, guest);
            if (managed) count_add(&t->managed, managed);
            thread_basic_info_data_t bi;
            mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
            if (thread_info(act, THREAD_BASIC_INFO,
                            (thread_info_t)&bi, &bc) == KERN_SUCCESS) {
                if (bi.run_state == TH_STATE_RUNNING) t->running++;
                else if (bi.run_state == TH_STATE_WAITING) t->waiting++;
                else t->other_state++;
            }
        }
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t)list, nlist * sizeof *list);
}

void kl_sample_report(FILE *out);   // below; the sampler self-reports periodically

static void *sampler_main(void *arg) {    (void)arg;
    const char *renv = getenv("KL_SAMPLE_REPORT_S");
    unsigned report_s = renv ? (unsigned)strtoul(renv, NULL, 10) : 30;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (!g_stop) {
        sample_once();
        // The report must not depend on an orderly exit: the interesting runs
        // are exactly the ones that die on a guest abort (kl_fatal_prepare
        // resets the signal handlers, so t_boot's fault reporter — and its
        // stop_report call — never runs). Print periodically from here; the
        // sampler thread is the only writer, so the counts are consistent.
        if (report_s) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - last.tv_sec >= (time_t)report_s) {
                kl_sample_report(stderr);
                last = now;
            }
        }
        usleep(g_interval_ms * 1000);
    }
    return NULL;
}

int kl_sample_start(unsigned interval_ms, const char *metadata_path) {
    if (g_started) return 1;
    if (interval_ms) g_interval_ms = interval_ms;
    g_main_act = mach_thread_self();
    kl_image *il2cpp = kl_find_image("libil2cpp.so");
    if (il2cpp && metadata_path)
        kl_il2cpp_resolver_init(il2cpp, metadata_path);
    else
        fprintf(stderr, "  [klepton] sampler: libil2cpp or metadata missing — "
                        "guest pcs will be module+offset only\n");
    g_stop = 0;
    if (pthread_create(&g_thread, NULL, sampler_main, NULL) != 0) {
        fprintf(stderr, "  [klepton] sampler: pthread_create failed\n");
        return 0;
    }
    g_started = 1;
    fprintf(stderr, "  [klepton] sampler: every %u ms, depth %d\n",
            g_interval_ms, KL_SAMPLE_DEPTH);
    return 1;
}

static void print_top(FILE *out, const char *title, counts *c,
                      uint64_t total, unsigned limit) {
    if (!c->n) return;
    qsort(c->v, c->n, sizeof *c->v, cmp_counts);
    fprintf(out, "%s\n", title);
    for (unsigned i = 0; i < c->n && i < limit; i++) {
        if (!c->v[i].n) break;
        fprintf(out, "  %5.1f%% (%5llu)  %s\n",
                total ? 100.0 * c->v[i].n / total : 0.0,
                (unsigned long long)c->v[i].n, c->v[i].label);
    }
}

void kl_sample_report(FILE *out) {
    if (!g_started) return;

    fprintf(out, "\n=== KL_SAMPLE: %llu samples @ %u ms, %u guest threads ===\n",
            (unsigned long long)g_nsamples, g_interval_ms, g_nthreads);
    uint64_t thread_samples = 0;
    for (unsigned i = 0; i < g_nthreads; i++) thread_samples += g_threads[i].samples;
    print_top(out, "-- top leaf frames (where threads were) --",
              &g_leaf, thread_samples, 25);
    print_top(out, "-- top first-guest frames (which guest code) --",
              &g_guest, thread_samples, 25);
    print_top(out, "-- top first-managed frames (which managed method) --",
              &g_managed, thread_samples, 25);

    fprintf(out, "-- per thread --\n");
    for (unsigned i = 0; i < g_nthreads; i++) {
        thread_rec *t = &g_threads[i];
        if (!t->samples) continue;
        // The informative per-thread label is the managed frame when the
        // thread ever reached managed code, else the raw leaf.
        counts *show = t->managed.n ? &t->managed : &t->leaf;
        qsort(show->v, show->n, sizeof *show->v, cmp_counts);
        fprintf(out, "  %-24s %5llu samples  run/wait/other %llu/%llu/%llu%s\n",
                t->name, (unsigned long long)t->samples,
                (unsigned long long)t->running, (unsigned long long)t->waiting,
                (unsigned long long)t->other_state,
                t->managed.n ? "" : "  (never in managed code)");
        for (unsigned k = 0; k < show->n && k < 3; k++)
            fprintf(out, "      %5.1f%%  %s\n", 100.0 * show->v[k].n / t->samples,
                    show->v[k].label);
    }
    fflush(out);
}

void kl_sample_stop_report(FILE *out) {
    if (!g_started) return;
    g_stop = 1;
    pthread_join(g_thread, NULL);
    kl_sample_report(out);
    g_started = 0;
}
