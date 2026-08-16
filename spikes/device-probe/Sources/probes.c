// Klepton on-device probe battery (visionOS).
// Batches every device-only unknown into one run.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include "probes.h"

// private, but the definitive answer on whether code-signing enforcement is relaxed
extern int csops(pid_t, unsigned int, void *, size_t);
#define CS_OPS_STATUS 0
#define CS_VALID 0x0000001
#define CS_GET_TASK_ALLOW 0x0000004
#define CS_HARD 0x0000100
#define CS_KILL 0x0000200
#define CS_ENFORCEMENT 0x0001000
#define CS_DEBUGGED 0x10000000

// ---------- tiny string builder ----------
typedef struct { char *p; size_t len, cap; } SB;
static void sb_init(SB *s){ s->cap=8192; s->len=0; s->p=malloc(s->cap); s->p[0]=0; }
static void sb(SB *s, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    char tmp[2048]; int n = vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
    if (n < 0) return;
    if (s->len + (size_t)n + 2 > s->cap) { s->cap = (s->len + n + 2) * 2; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->len, tmp, n); s->len += n; s->p[s->len++] = '\n'; s->p[s->len] = 0;
}

// ---------- system register access ----------
static inline uint64_t rd_tpidrro(void){ uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0":"=r"(v)); return v; }
static inline uint64_t rd_tpidr(void)  { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0"  :"=r"(v)); return v; }
static inline void     wr_tpidr(uint64_t v){ __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }
// exactly what a klepton-ld-rewritten guest stack-protector prologue executes
static inline uint64_t guest_canary(void){
    uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0\n\tldr %0, [%0, #40]":"=r"(v)); return v;
}

unsigned long long klepton_tsd_slot(int i){ return ((uint64_t*)rd_tpidrro())[i]; }

// ---------- SIGILL guard, to detect trapped MRS ----------
static sigjmp_buf g_jb; static volatile sig_atomic_t g_trapped;
static void on_sigill(int s){ (void)s; g_trapped = 1; siglongjmp(g_jb, 1); }

#define GUARDED(body, okfmt, ...) do {                                   \
    struct sigaction na, oa; memset(&na,0,sizeof na);                    \
    na.sa_handler = on_sigill; sigemptyset(&na.sa_mask);                 \
    sigaction(SIGILL, &na, &oa); g_trapped = 0;                          \
    if (sigsetjmp(g_jb, 1) == 0) { body; sb(s, okfmt, ##__VA_ARGS__); }  \
    else sb(s, "    !! TRAPPED (SIGILL)");                               \
    sigaction(SIGILL, &oa, NULL);                                        \
} while (0)

// ---------- P5: canary mechanism under preemption ----------
typedef struct { uint64_t want; long mismatches; int rounds; } CTX;
static void *canary_worker(void *arg) {
    CTX *c = arg;
    ((uint64_t*)rd_tpidrro())[5] = c->want;      // klepton_thread_init()
    volatile long sink = 0;
    for (int r = 0; r < c->rounds; r++) {
        for (volatile long i = 0; i < 2000000L; i++) sink += i;  // force preemption
        usleep(400);                                              // force ctx switch
        if (guest_canary() != c->want) c->mismatches++;
    }
    return NULL;
}

// ---------- P6: dlopen an embedded framework ----------
static void probe_dlopen(SB *s, const char *bundle, const char *fw, const char *note) {
    char path[1024];
    snprintf(path, sizeof path, "%s/Frameworks/%s.framework/%s", bundle, fw, fw);
    sb(s, "  %s  (%s)", fw, note);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { sb(s, "    dlopen FAILED: %s", dlerror()); return; }
    sb(s, "    dlopen OK  handle=%p", h);
    int (*fn)(void) = (int(*)(void))dlsym(h, "klepton_probe_value");
    if (!fn) { sb(s, "    dlsym FAILED: %s", dlerror()); dlclose(h); return; }
    int v = fn();
    sb(s, "    dlsym+call OK -> %d %s", v, v == 0x4B4C ? "[CORRECT]" : "[WRONG VALUE]");
    dlclose(h);
}

// ---------- P12: does a HAND-EMITTED dylib pass AMFI? ----------
//
// P7 above answers that a signed dylib in an embedded framework loads. It says
// nothing about klepton-ld's output, because those frameworks came out of clang
// and ld64. This
// one is klepton-ld's output — a Mach-O no Apple linker ever touched, carrying a
// guest ELF image verbatim in __TEXT,__klelf, with no chained fixups, no exports
// trie and an empty symbol table. That is the shape AMFI has every reason to
// refuse, and it is the single highest-information experiment here.
//
// It goes past loading and actually *runs* the guest, because "it mapped" and
// "it executes" are different claims and only the second one matters. The ELF
// relocation walk is duplicated here rather than reused from runtime/kl_image.c
// on purpose: this app must build with nothing but the visionOS SDK, and the
// runtime does not yet (mach_vm.h is #error unsupported there). The counts are
// printed so they can be checked against the host's known 955/430/98/312 — if
// the probe walked a different set of relocations, that shows up as a mismatch
// rather than as a plausible-looking pass.
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } E_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } E_Phdr;
typedef struct { uint64_t d_tag, d_val; } E_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } E_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } E_Rela;

static void guest_unresolved(void) { }   // parked: named imports we cannot bind

static void probe_klepton_ld(SB *s, const char *bundle) {
    char path[1024];
    snprintf(path, sizeof path, "%s/Frameworks/KleptonGuest.framework/KleptonGuest", bundle);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { sb(s, "    dlopen FAILED: %s", dlerror());
              sb(s, "    >>> AMFI REJECTED THE HAND-EMITTED DYLIB"); return; }
    sb(s, "    dlopen OK  handle=%p   <<< AMFI ACCEPTED IT", h);

    // Find the mach_header by matching dyld's loaded-image list: the dylib
    // deliberately exports nothing, so dlsym is not available to us.
    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "KleptonGuest.framework")) {
            mh = (const struct mach_header_64 *)_dyld_get_image_header(i); break;
        }
    }
    if (!mh) { sb(s, "    loaded but not in dyld's image list"); return; }

    unsigned long secsz = 0;
    uint8_t *elf = getsectiondata(mh, "__TEXT", "__klelf", &secsz);
    if (!elf) { sb(s, "    no __TEXT,__klelf section"); return; }
    sb(s, "    __klelf at %p (%lu bytes), ELF magic %s",
       (void *)elf, secsz, memcmp(elf, "\177ELF", 4) == 0 ? "OK" : "BAD");
    if (memcmp(elf, "\177ELF", 4) != 0) return;

    unsigned long stsz = 0;
    const uint32_t *st = (const uint32_t *)getsectiondata(mh, "__TEXT", "__klstat", &stsz);
    if (st && stsz >= 24 && st[0] == 0x38315838u)
        sb(s, "    __klstat: x18 sites=%u veneered=%u refused=%u  TLS rewrites=%u  slot=%u",
           st[1], st[2], st[3], st[4], st[5]);

    // W^X: the guest text must be mapped read-execute and NOT writable. If this
    // says writable, the whole reason for the translated path has quietly evaporated.
    { vm_address_t a = (vm_address_t)elf; vm_size_t sz = 0;
      vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t obj;
      if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS)
          sb(s, "    guest text protection = %c%c%c  %s",
             info.protection & VM_PROT_READ ? 'r' : '-',
             info.protection & VM_PROT_WRITE ? 'w' : '-',
             info.protection & VM_PROT_EXECUTE ? 'x' : '-',
             (info.protection & VM_PROT_WRITE) ? "<<< WRITABLE — W^X NOT HELD" : "[W^X held]");
    }

    // ---- relocate, exactly as the runtime does ----
    const E_Ehdr *eh = (const E_Ehdr *)elf;
    const E_Phdr *ph = (const E_Phdr *)(elf + eh->e_phoff);
    const E_Sym *symtab = NULL; const char *strtab = NULL;
    const E_Rela *rela = NULL, *jmprel = NULL;
    uint64_t relasz = 0, pltsz = 0;
    void (**init_array)(void) = NULL; uint64_t init_count = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 2 /*PT_DYNAMIC*/) continue;
        for (const E_Dyn *d = (const E_Dyn *)(elf + ph[i].p_vaddr); d->d_tag; d++)
            switch (d->d_tag) {
            case 6:  symtab = (const E_Sym *)(elf + d->d_val); break;
            case 5:  strtab = (const char *)(elf + d->d_val); break;
            case 7:  rela   = (const E_Rela *)(elf + d->d_val); break;
            case 8:  relasz = d->d_val; break;
            case 23: jmprel = (const E_Rela *)(elf + d->d_val); break;
            case 2:  pltsz  = d->d_val; break;
            case 25: init_array = (void (**)(void))(elf + d->d_val); break;
            case 27: init_count = d->d_val / 8; break;
            }
    }
    if (!symtab || !strtab) { sb(s, "    missing DT_SYMTAB/DT_STRTAB"); return; }

    unsigned nrel = 0, nabs = 0, nglob = 0, njmp = 0, nbound = 0, nstub = 0;
    for (int pass = 0; pass < 2; pass++) {
        const E_Rela *r = pass ? jmprel : rela;
        uint64_t n = (pass ? pltsz : relasz) / sizeof(E_Rela);
        for (uint64_t i = 0; r && i < n; i++) {
            uint64_t *slot = (uint64_t *)(elf + r[i].r_offset);
            uint32_t type = (uint32_t)r[i].r_info, sidx = (uint32_t)(r[i].r_info >> 32);
            if (type == 1027) { *slot = (uint64_t)(uintptr_t)elf + r[i].r_addend; nrel++; continue; }
            if (type != 257 && type != 1025 && type != 1026) continue;
            const E_Sym *sy = &symtab[sidx];
            uint64_t v;
            if (sy->st_shndx) v = (uint64_t)(uintptr_t)(elf + sy->st_value);
            else {
                // The guest's 42 imports are ordinary libc, which Darwin has.
                // The few that are bionic-only get a parked stub so the count is
                // honest rather than the load silently half-done.
                void *e = dlsym(RTLD_DEFAULT, strtab + sy->st_name);
                if (e) { v = (uint64_t)(uintptr_t)e; nbound++; }
                else   { v = (uint64_t)(uintptr_t)guest_unresolved; nstub++; }
            }
            *slot = v + r[i].r_addend;
            if (type == 257) nabs++; else if (type == 1025) nglob++; else njmp++;
        }
    }
    sb(s, "    relocations: RELATIVE=%u ABS64=%u GLOB_DAT=%u JUMP_SLOT=%u (total %u)",
       nrel, nabs, nglob, njmp, nrel + nabs + nglob + njmp);
    sb(s, "      host expects 955/430/98/312 = 1795 %s",
       (nrel == 955 && nabs == 430 && nglob == 98 && njmp == 312) ? "[MATCH]" : "[MISMATCH]");
    sb(s, "    imports: %u bound via dlsym, %u parked (bionic-only)", nbound, nstub);

    for (uint64_t i = 0; i < init_count; i++) if (init_array[i]) init_array[i]();
    sb(s, "    DT_INIT_ARRAY ran (%llu entries)", (unsigned long long)init_count);

    // ---- execute guest code: the P1 exit criterion, on rung 3 ----
    const char *(*version)(void) = NULL; int (*enc_size)(int) = NULL;
    void *(*enc_create)(int,int,int,int*) = NULL, *(*dec_create)(int,int,int*) = NULL;
    int (*encode)(void*,const short*,int,unsigned char*,int) = NULL;
    int (*decode)(void*,const unsigned char*,int,short*,int,int) = NULL;
    size_t nsym = ((const char *)strtab - (const char *)symtab) / sizeof(E_Sym);
    for (size_t i = 0; i < nsym; i++) {
        const E_Sym *sy = &symtab[i];
        if (!sy->st_shndx || !sy->st_name) continue;
        const char *nm = strtab + sy->st_name;
        void *a = elf + sy->st_value;
        if (!strcmp(nm, "opus_get_version_string")) version = a;
        else if (!strcmp(nm, "opus_encoder_get_size")) enc_size = a;
        else if (!strcmp(nm, "opus_encoder_create")) enc_create = a;
        else if (!strcmp(nm, "opus_decoder_create")) dec_create = a;
        else if (!strcmp(nm, "opus_encode")) encode = a;
        else if (!strcmp(nm, "opus_decode")) decode = a;
    }
    if (!version || !enc_size) { sb(s, "    guest exports not found"); return; }
    sb(s, "    guest call: opus_get_version_string() = \"%s\"", version());
    sb(s, "    guest call: opus_encoder_get_size(2)  = %d %s",
       enc_size(2), enc_size(2) == 48484 ? "[matches host]" : "[DIFFERS from host 48484]");

    if (!enc_create || !dec_create || !encode || !decode) return;
    int e = 0;
    void *enc = enc_create(48000, 2, 2049, &e);
    void *dec = dec_create(48000, 2, &e);
    if (!enc || !dec) { sb(s, "    encoder/decoder create failed (%d)", e); return; }
    static short pcm[960 * 2]; static unsigned char pkt[4000]; static short out[960 * 2];
    for (int i = 0; i < 960; i++) {
        short v = (short)(12000.0 * __builtin_sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
        pcm[i * 2] = v; pcm[i * 2 + 1] = v;
    }
    int nb = encode(enc, pcm, 960, pkt, sizeof pkt);
    int ns = nb > 0 ? decode(dec, pkt, nb, out, 960, 0) : -1;
    double energy = 0;
    for (int i = 0; i < (ns > 0 ? ns * 2 : 0); i++) energy += (double)out[i] * out[i];
    if (ns > 0) energy /= (ns * 2);
    sb(s, "    guest roundtrip: encode -> %d bytes, decode -> %d samples, mean energy %.0f",
       nb, ns, energy);
    // Deliberately does not say "on device" — this same code is run on the host
    // and in the simulator to validate the probe itself, and a message that
    // claims the platform would read as a device result wherever it appeared.
    sb(s, "    %s", (ns == 960 && energy > 1e5)
        ? ">>> GUEST CODE RAN CORRECTLY FROM A SIGNED HAND-EMITTED DYLIB"
        : ">>> ROUNDTRIP WRONG — loaded and executed, but the result is not right");
}

char *klepton_run_probes(const char *bundle_path) {
    SB S; SB *s = &S; sb_init(s);
    sb(s, "===== KLEPTON DEVICE PROBE =====");

    // ---- P1 environment ----
    sb(s, "\n[P1] environment");
    sb(s, "    getpagesize()   = %d", getpagesize());
    sb(s, "    vm_page_size    = %lu", (unsigned long)vm_page_size);
    sb(s, "    sizeof(void*)   = %zu", sizeof(void*));
    sb(s, "    pthread_mutex_t = %zu bytes  (bionic = 4)", sizeof(pthread_mutex_t));
    sb(s, "    sem_t           = %zu bytes", sizeof(sem_t));

    // ---- P2 system registers readable? ----
    sb(s, "\n[P2] system register access from EL0");
    { uint64_t v = 0; GUARDED(v = rd_tpidrro(), "    mrs tpidrro_el0 = %016llx  [OK]", (unsigned long long)v); }
    { uint64_t v = 0; GUARDED(v = rd_tpidr(),   "    mrs tpidr_el0   = %016llx  [OK]", (unsigned long long)v); }

    // ---- P3 Darwin TSD layout ----
    sb(s, "\n[P3] Darwin TSD via TPIDRRO_EL0  (bionic slot 5 = STACK_GUARD)");
    uint64_t tp = rd_tpidrro();
    sb(s, "    pthread_self = %016llx   TPIDRRO = %016llx  (delta 0x%llx)",
       (unsigned long long)(uintptr_t)pthread_self(), (unsigned long long)tp,
       (unsigned long long)(tp - (uintptr_t)pthread_self()));
    for (int i = 0; i < 9; i++)
        sb(s, "      slot %d (+%2d) = %016llx%s", i, i*8,
           (unsigned long long)((uint64_t*)tp)[i], i == 5 ? "   <== must be FREE" : "");
    sb(s, "    VERDICT: slot 5 %s", ((uint64_t*)tp)[5] == 0 ? "is ZERO [FREE - good]" : "is IN USE [PROBLEM]");

    // ---- P4 TPIDR_EL0 clobber behaviour (does the macOS finding hold?) ----
    sb(s, "\n[P4] TPIDR_EL0 volatility");
    { const uint64_t SENT = 0xDEADBEEF12340000ULL;
      uint64_t o = rd_tpidr(); wr_tpidr(SENT); uint64_t a = rd_tpidr(); wr_tpidr(o);
      sb(s, "    write/readback   : %s", a == SENT ? "survived [WRITABLE]" : "clobbered");
      wr_tpidr(SENT); usleep(2000); uint64_t b = rd_tpidr(); wr_tpidr(o);
      sb(s, "    across usleep    : %s", b == SENT ? "survived" : "CLOBBERED (expected)");
      sb(s, "    -> TPIDR_EL0 %s", b == SENT ? "may be usable (differs from macOS!)"
                                             : "unusable, as on macOS. Rewrite to TPIDRRO stands."); }

    // ---- P5 the actual klepton-ld TLS fix, under preemption ----
    sb(s, "\n[P5] TPIDRRO_EL0 + slot 5 canary under preemption (the klepton-ld fix)");
    { enum { NT = 4 }; pthread_t t[NT]; CTX c[NT]; long bad = 0;
      for (int i = 0; i < NT; i++) { c[i].want = 0xC0FFEE0000ULL + i; c[i].mismatches = 0; c[i].rounds = 12;
                                     pthread_create(&t[i], NULL, canary_worker, &c[i]); }
      for (int i = 0; i < NT; i++) { pthread_join(t[i], NULL); bad += c[i].mismatches; }
      sb(s, "    4 threads x 12 rounds, forced preemption + usleep");
      sb(s, "    VERDICT: %ld mismatches %s", bad, bad == 0 ? "[MECHANISM WORKS]" : "[BROKEN]"); }

    // ---- P6 W^X: confirms the AOT premise ----
    sb(s, "\n[P6] W^X / executable memory (confirms AOT premise)");
    { size_t L = 65536;
      void *rw = mmap(NULL, L, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
      sb(s, "    mmap RW            : %s", rw != MAP_FAILED ? "OK" : strerror(errno));
      if (rw != MAP_FAILED) {
        int r = mprotect(rw, L, PROT_READ|PROT_EXEC);
        sb(s, "    mprotect R+X       : %s", r == 0 ? "ALLOWED (!! unexpected)" : strerror(errno));
        munmap(rw, L);
      }
      void *wx = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
      sb(s, "    mmap RWX           : %s", wx != MAP_FAILED ? "ALLOWED (!! unexpected)" : strerror(errno));
      if (wx != MAP_FAILED) munmap(wx, L);
      void *jit = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
      sb(s, "    mmap RWX+MAP_JIT   : %s", jit != MAP_FAILED ? "ALLOWED" : strerror(errno));
      if (jit != MAP_FAILED) munmap(jit, L); }

    // ---- P7 dlopen embedded frameworks ----
    sb(s, "\n[P7] dlopen of embedded frameworks ");
    sb(s, "    bundle: %s", bundle_path ? bundle_path : "(null)");
    if (bundle_path) {
        probe_dlopen(s, bundle_path, "KleptonProbeA", "default segment alignment");
        probe_dlopen(s, bundle_path, "KleptonProbeB", "64KB segalign, as translated guest libs");
    }

    // ---- P11 does RWX memory actually EXECUTE? (P6 only tested mapping creation) ----
    sb(s, "\n[P11] can we actually EXECUTE from anonymous memory?");
    { unsigned int st = 0;
      if (csops(getpid(), CS_OPS_STATUS, &st, sizeof st) == 0) {
        sb(s, "    csops status = 0x%08x", st);
        sb(s, "      CS_VALID=%d CS_HARD=%d CS_KILL=%d CS_ENFORCEMENT=%d",
           !!(st&CS_VALID), !!(st&CS_HARD), !!(st&CS_KILL), !!(st&CS_ENFORCEMENT));
        sb(s, "      CS_GET_TASK_ALLOW=%d  CS_DEBUGGED=%d  <== if DEBUGGED=1, results below are NOT representative",
           !!(st&CS_GET_TASK_ALLOW), !!(st&CS_DEBUGGED));
      } else sb(s, "    csops failed: %s", strerror(errno));

      struct kinfo_proc kp; size_t kl = sizeof kp;
      int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
      if (sysctl(mib, 4, &kp, &kl, NULL, 0) == 0)
        sb(s, "    debugger attached (P_TRACED) = %d", !!(kp.kp_proc.p_flag & P_TRACED));
    }
    // A codesigning violation is SIGKILL and cannot be caught -> must fork.
    { static const uint32_t code[2] = { 0x52896980u /* mov w0,#0x4B4C */, 0xd65f03c0u /* ret */ };
      const char *modes[2] = { "mmap RW then mprotect RX", "mmap RWX directly" };
      for (int mode = 0; mode < 2; mode++) {
        pid_t pid = fork();
        if (pid == 0) {
            size_t L = 16384; void *m;
            if (mode == 0) {
                m = mmap(NULL, L, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
                if (m == MAP_FAILED) _exit(10);
                memcpy(m, code, sizeof code);
                if (mprotect(m, L, PROT_READ|PROT_EXEC) != 0) _exit(11);
            } else {
                m = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
                if (m == MAP_FAILED) _exit(10);
                memcpy(m, code, sizeof code);
            }
            __builtin___clear_cache((char*)m, (char*)m + sizeof code);
            int (*fn)(void) = (int(*)(void))m;
            int v = fn();                       // <-- the actual test
            _exit(v == 0x4B4C ? 0 : 12);
        }
        int st2 = 0; waitpid(pid, &st2, 0);
        if (WIFSIGNALED(st2)) {
            int sg = WTERMSIG(st2);
            sb(s, "    %-26s : KILLED by signal %d (%s) -> EXECUTION BLOCKED",
               modes[mode], sg, sg == 9 ? "SIGKILL/codesigning" : strsignal(sg));
        } else {
            int rc = WEXITSTATUS(st2);
            sb(s, "    %-26s : %s", modes[mode],
               rc == 0  ? "EXECUTED SUCCESSFULLY  <<< W^X IS NOT ENFORCED"
             : rc == 10 ? "mmap failed"
             : rc == 11 ? "mprotect failed -> blocked at mprotect"
             : rc == 12 ? "ran but returned wrong value" : "unknown");
        }
      }
    }

    // ---- P12 the hand-emitted-dylib question ----
    sb(s, "\n[P12] klepton-ld hand-emitted dylib under AMFI");
    if (bundle_path) probe_klepton_ld(s, bundle_path);

    sb(s, "\n===== END =====");
    return S.p;
}

// ============================ P13 ============================
// ANGLE under AMFI, and the Metal interop primitive.
//
// Two questions in one, because they need the same setup and the device is the
// only place either can be answered:
//
//   1. does a `vtool`-retargeted third-party dylib pass AMFI? This is the last
//      unverified tail of the ANGLE retarget. The link line and all 299 undefined
//      symbols were checked against the xrOS SDK offline; what no
//      host-side check can establish is whether AMFI objects to a Mach-O whose
//      LC_BUILD_VERSION was rewritten after signing-time-1 by a stock tool.
//   2. does eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) work *here*? It works on
//      macOS — `make mtltex`, every check green including per-eye array
//      slices and RGBA16F at the guest's real 2198x2304. visionOS is a different
//      Metal and a different sandbox.
//
// The texture comes from Swift, deliberately: that is the shipping seam
// (Swift owns Metal, C owns the guest), so this rehearses the real
// arrangement rather than a convenient one. C hands Swift ANGLE's MTLDevice,
// Swift allocates on it and hands the texture back, and each side verifies the
// pixels through its own API — GL says it wrote them, Metal says they landed in
// our texture. Neither claim implies the other.
//
// The ANGLE loader is duplicated here rather than shared with kl_glfb.c for the
// same reason probe_klepton_ld duplicates the relocation walk: this app builds
// against nothing but the visionOS SDK.

#define EGL_NO_CONTEXT           ((void *)0)
#define EGL_NO_IMAGE             ((void *)0)
#define EGL_DEFAULT_DISPLAY      ((void *)0)
#define EGL_NONE                 0x3038
#define EGL_WIDTH                0x3057
#define EGL_HEIGHT               0x3056
#define EGL_SURFACE_TYPE         0x3033
#define EGL_PBUFFER_BIT          0x0001
#define EGL_RENDERABLE_TYPE      0x3040
#define EGL_OPENGL_ES3_BIT       0x0040
#define EGL_RED_SIZE             0x3024
#define EGL_GREEN_SIZE           0x3023
#define EGL_BLUE_SIZE            0x3022
#define EGL_ALPHA_SIZE           0x3021
#define EGL_EXTENSIONS           0x3055
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE            0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE       0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_DEVICE_EXT                      0x322C
#define EGL_METAL_DEVICE_ANGLE              0x34A6
#define EGL_METAL_TEXTURE_ANGLE             0x34A7
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE 0x34DD
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE   0x345D

#define GL_NO_ERROR              0
#define GL_TEXTURE_2D            0x0DE1
#define GL_FRAMEBUFFER           0x8D40
#define GL_COLOR_ATTACHMENT0     0x8CE0
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_RGBA                  0x1908
#define GL_RGBA16F               0x881A
#define GL_UNSIGNED_BYTE         0x1401
#define GL_FLOAT                 0x1406
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_ARRAY_BUFFER          0x8892
#define GL_STATIC_DRAW           0x88E4
#define GL_TRIANGLES             0x0004
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02

static SB g_alog;                       // P13's own report, read back by Swift
static void *g_a_egl, *g_a_gles;
static void *g_a_dpy, *g_a_ctx, *g_a_cfg;
static uint32_t g_a_fbo, g_a_prog;

static void *asym(const char *n) {
    void *p = g_a_gles ? dlsym(g_a_gles, n) : NULL;
    if (!p && g_a_egl) p = dlsym(g_a_egl, n);
    return p;
}

static void *(*a_eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *);
static unsigned (*a_eglInitialize)(void *, int32_t *, int32_t *);
static unsigned (*a_eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
static void *(*a_eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*a_eglCreatePbufferSurface)(void *, void *, const int32_t *);
static unsigned (*a_eglMakeCurrent)(void *, void *, void *, void *);
static const char *(*a_eglQueryString)(void *, int32_t);
static const char *(*a_eglQueryDeviceStringEXT)(void *, int32_t);
static unsigned (*a_eglQueryDisplayAttribEXT)(void *, int32_t, intptr_t *);
static unsigned (*a_eglQueryDeviceAttribEXT)(void *, int32_t, intptr_t *);
static void *(*a_eglCreateImageKHR)(void *, void *, uint32_t, void *, const int32_t *);
static unsigned (*a_eglDestroyImageKHR)(void *, void *);
static uint32_t (*a_eglGetError)(void);
static void (*a_glEGLImageTargetTexture2DOES)(uint32_t, void *);
static void (*a_glGenTextures)(int32_t, uint32_t *);
static void (*a_glBindTexture)(uint32_t, uint32_t);
static void (*a_glGenFramebuffers)(int32_t, uint32_t *);
static void (*a_glBindFramebuffer)(uint32_t, uint32_t);
static void (*a_glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
static void (*a_glClearColor)(float, float, float, float);
static void (*a_glClear)(uint32_t);
static void (*a_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
static void (*a_glFinish)(void);
static uint32_t (*a_glGetError)(void);
static void (*a_glViewport)(int32_t, int32_t, int32_t, int32_t);
static const uint8_t *(*a_glGetString)(uint32_t);
static uint32_t (*a_glCreateShader)(uint32_t);
static void (*a_glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
static void (*a_glCompileShader)(uint32_t);
static void (*a_glGetShaderiv)(uint32_t, uint32_t, int32_t *);
static uint32_t (*a_glCreateProgram)(void);
static void (*a_glAttachShader)(uint32_t, uint32_t);
static void (*a_glLinkProgram)(uint32_t);
static void (*a_glGetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*a_glUseProgram)(uint32_t);
static void (*a_glGenBuffers)(int32_t, uint32_t *);
static void (*a_glBindBuffer)(uint32_t, uint32_t);
static void (*a_glBufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void (*a_glVertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *);
static void (*a_glEnableVertexAttribArray)(uint32_t);
static void (*a_glDrawArrays)(uint32_t, int32_t, int32_t);
static void (*a_glUniform3f)(int32_t, float, float, float);
static int32_t (*a_glGetUniformLocation)(uint32_t, const char *);

const char *klepton_angle_log(void) { return g_alog.p ? g_alog.p : ""; }

// Returns ANGLE's own MTLDevice as an opaque pointer, or NULL. The device has to
// be ANGLE's: the extension spec requires the texture come from the display's
// device, so MTLCreateSystemDefaultDevice() is not interchangeable and would
// fail with EGL_BAD_PARAMETER for a reason nothing else would explain.
void *klepton_angle_init(const char *bundle_path) {
    SB *s = &g_alog;
    if (!s->p) sb_init(s);

    char p[1024];
    snprintf(p, sizeof p, "%s/Frameworks/libEGL.framework/libEGL", bundle_path);
    g_a_egl = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!g_a_egl) {
        sb(s, "    dlopen libEGL FAILED: %s", dlerror());
        sb(s, "    >>> AMFI REJECTED THE vtool-RETARGETED DYLIB");
        return NULL;
    }
    sb(s, "    dlopen libEGL OK       <<< AMFI ACCEPTED THE RETARGETED DYLIB");
    // libEGL finds libGLESv2 itself on iOS-family platforms — it dlopens
    // <exec dir>/Frameworks/libGLESv2.framework/libGLESv2 (ANGLE's
    // system_utils_posix.cpp), which is exactly where the embed phase puts it.
    // We open it too, for the gl* exports: libEGL exports only egl* (115
    // symbols) and libGLESv2 only gl* (826), so both handles are needed.
    snprintf(p, sizeof p, "%s/Frameworks/libGLESv2.framework/libGLESv2", bundle_path);
    g_a_gles = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!g_a_gles) { sb(s, "    dlopen libGLESv2 FAILED: %s", dlerror()); return NULL; }
    sb(s, "    dlopen libGLESv2 OK    (12 MB, carries all of libANGLE)");

    a_eglGetPlatformDisplayEXT = asym("eglGetPlatformDisplayEXT");
    a_eglInitialize            = asym("eglInitialize");
    a_eglChooseConfig          = asym("eglChooseConfig");
    a_eglCreateContext         = asym("eglCreateContext");
    a_eglCreatePbufferSurface  = asym("eglCreatePbufferSurface");
    a_eglMakeCurrent           = asym("eglMakeCurrent");
    a_eglQueryString           = asym("eglQueryString");
    a_eglQueryDeviceStringEXT  = asym("eglQueryDeviceStringEXT");
    a_eglQueryDisplayAttribEXT = asym("eglQueryDisplayAttribEXT");
    a_eglQueryDeviceAttribEXT  = asym("eglQueryDeviceAttribEXT");
    a_eglCreateImageKHR        = asym("eglCreateImageKHR");
    a_eglDestroyImageKHR       = asym("eglDestroyImageKHR");
    a_eglGetError              = asym("eglGetError");
    a_glEGLImageTargetTexture2DOES = asym("glEGLImageTargetTexture2DOES");
    a_glGenTextures            = asym("glGenTextures");
    a_glBindTexture            = asym("glBindTexture");
    a_glGenFramebuffers        = asym("glGenFramebuffers");
    a_glBindFramebuffer        = asym("glBindFramebuffer");
    a_glFramebufferTexture2D   = asym("glFramebufferTexture2D");
    a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
    a_glClearColor             = asym("glClearColor");
    a_glClear                  = asym("glClear");
    a_glReadPixels             = asym("glReadPixels");
    a_glFinish                 = asym("glFinish");
    a_glGetError               = asym("glGetError");
    a_glViewport               = asym("glViewport");
    a_glGetString              = asym("glGetString");
    a_glCreateShader           = asym("glCreateShader");
    a_glShaderSource           = asym("glShaderSource");
    a_glCompileShader          = asym("glCompileShader");
    a_glGetShaderiv            = asym("glGetShaderiv");
    a_glCreateProgram          = asym("glCreateProgram");
    a_glAttachShader           = asym("glAttachShader");
    a_glLinkProgram            = asym("glLinkProgram");
    a_glGetProgramiv           = asym("glGetProgramiv");
    a_glUseProgram             = asym("glUseProgram");
    a_glGenBuffers             = asym("glGenBuffers");
    a_glBindBuffer             = asym("glBindBuffer");
    a_glBufferData             = asym("glBufferData");
    a_glVertexAttribPointer    = asym("glVertexAttribPointer");
    a_glEnableVertexAttribArray = asym("glEnableVertexAttribArray");
    a_glDrawArrays             = asym("glDrawArrays");
    a_glUniform3f              = asym("glUniform3f");
    a_glGetUniformLocation     = asym("glGetUniformLocation");
    if (!a_eglInitialize || !a_eglCreateImageKHR || !a_glEGLImageTargetTexture2DOES
        || !a_eglQueryDeviceAttribEXT || !a_glFramebufferTexture2D) {
        sb(s, "    missing an interop entry point (createImage=%d imageTarget=%d devAttrib=%d)",
           a_eglCreateImageKHR != NULL, a_glEGLImageTargetTexture2DOES != NULL,
           a_eglQueryDeviceAttribEXT != NULL);
        return NULL;
    }

    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    g_a_dpy = a_eglGetPlatformDisplayEXT
            ? a_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs)
            : NULL;
    int32_t major = 0, minor = 0;
    if (!g_a_dpy || !a_eglInitialize(g_a_dpy, &major, &minor)) {
        sb(s, "    eglInitialize FAILED on the Metal backend (EGL error 0x%x)",
           a_eglGetError ? a_eglGetError() : 0);
        return NULL;
    }
    sb(s, "    EGL %d.%d initialised on the Metal backend", major, minor);

    const char *exts = a_eglQueryString(g_a_dpy, EGL_EXTENSIONS);
    static const char *want[] = { "EGL_ANGLE_metal_texture_client_buffer",
                                  "EGL_KHR_image_base",
                                  "EGL_ANGLE_metal_shared_event_sync" };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        sb(s, "    %-42s %s", want[i],
           exts && strstr(exts, want[i]) ? "present" : "MISSING");

    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    int32_t ncfg = 0;
    if (!a_eglChooseConfig(g_a_dpy, cfg_attrs, &g_a_cfg, 1, &ncfg) || ncfg < 1) {
        sb(s, "    eglChooseConfig FAILED"); return NULL;
    }
    const int32_t surf_attrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    void *surf = a_eglCreatePbufferSurface(g_a_dpy, g_a_cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_a_ctx = a_eglCreateContext(g_a_dpy, g_a_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!g_a_ctx || !a_eglMakeCurrent(g_a_dpy, surf, surf, g_a_ctx)) {
        sb(s, "    no current ES3 context (EGL error 0x%x)",
           a_eglGetError ? a_eglGetError() : 0);
        return NULL;
    }
    sb(s, "    GL_RENDERER = %s", (const char *)a_glGetString(GL_RENDERER));
    sb(s, "    GL_VERSION  = %s", (const char *)a_glGetString(GL_VERSION));

    intptr_t egl_dev = 0, mtl_dev = 0;
    if (!a_eglQueryDisplayAttribEXT(g_a_dpy, EGL_DEVICE_EXT, &egl_dev) || !egl_dev) {
        sb(s, "    eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) FAILED"); return NULL;
    }
    // EGL_ANGLE_device_metal is a *device* extension, so it is advertised here
    // and never in the display string. Looking for it in the display string
    // reports a working capability as missing — cost one confused minute on the
    // host, so it is spelled out.
    const char *dexts = a_eglQueryDeviceStringEXT
                      ? a_eglQueryDeviceStringEXT((void *)egl_dev, EGL_EXTENSIONS) : NULL;
    sb(s, "    EGL_ANGLE_device_metal (device ext)        %s",
       dexts && strstr(dexts, "EGL_ANGLE_device_metal") ? "present" : "MISSING");
    if (!a_eglQueryDeviceAttribEXT((void *)egl_dev, EGL_METAL_DEVICE_ANGLE, &mtl_dev)
        || !mtl_dev) {
        sb(s, "    eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE) FAILED"); return NULL;
    }
    a_glGenFramebuffers(1, &g_a_fbo);
    return (void *)mtl_dev;
}

// A triangle covering the whole clip square, in `rgb`. A clear can be serviced
// by a load-action alone; a draw needs the whole pipeline pointed at our
// texture, which is the claim worth making.
static int angle_draw_tri(float r, float g, float b) {
    if (!g_a_prog) {
        const char *vs = "#version 300 es\nin vec2 p;void main(){gl_Position=vec4(p,0.,1.);}\n";
        const char *fs = "#version 300 es\nprecision mediump float;uniform vec3 c;"
                         "out vec4 o;void main(){o=vec4(c,1.);}\n";
        uint32_t v = a_glCreateShader(GL_VERTEX_SHADER), f = a_glCreateShader(GL_FRAGMENT_SHADER);
        int32_t ok = 0;
        a_glShaderSource(v, 1, &vs, NULL); a_glCompileShader(v);
        a_glGetShaderiv(v, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        a_glShaderSource(f, 1, &fs, NULL); a_glCompileShader(f);
        a_glGetShaderiv(f, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        g_a_prog = a_glCreateProgram();
        a_glAttachShader(g_a_prog, v); a_glAttachShader(g_a_prog, f);
        a_glLinkProgram(g_a_prog);
        a_glGetProgramiv(g_a_prog, GL_LINK_STATUS, &ok); if (!ok) { g_a_prog = 0; return 0; }
        uint32_t vbo = 0;
        const float tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
        a_glGenBuffers(1, &vbo);
        a_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        a_glBufferData(GL_ARRAY_BUFFER, (intptr_t)sizeof tri, tri, GL_STATIC_DRAW);
        a_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
        a_glEnableVertexAttribArray(0);
    }
    a_glUseProgram(g_a_prog);
    if (a_glUniform3f && a_glGetUniformLocation)
        a_glUniform3f(a_glGetUniformLocation(g_a_prog, "c"), r, g, b);
    while (a_glGetError() != GL_NO_ERROR) {}
    a_glDrawArrays(GL_TRIANGLES, 0, 3);
    return a_glGetError() == GL_NO_ERROR;
}

// Wrap `slice` of the Swift-allocated MTLTexture as a GL texture and draw into
// it. `f16` selects the guest's real eye format (GL_RGBA16F) over RGBA8; the
// RGBA8 case additionally reads back through GL, which a float target cannot do
// with a fixed format/type pair.
// Returns 0 on success, non-zero at the step that failed.
int klepton_angle_draw(void *mtl_texture, int slice, int f16, int w, int h,
                       float r, float g, float b) {
    SB *s = &g_alog;
    if (!g_a_dpy || !mtl_texture) return 1;
    uint32_t ifmt = f16 ? GL_RGBA16F : GL_RGBA;
    const int32_t img_attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE, slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,   (int32_t)ifmt,
        EGL_NONE,
    };
    void *img = a_eglCreateImageKHR(g_a_dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                    mtl_texture, img_attrs);
    if (img == EGL_NO_IMAGE) {
        sb(s, "    slice %d: eglCreateImageKHR FAILED, EGL error 0x%x", slice,
           a_eglGetError ? a_eglGetError() : 0);
        return 2;
    }
    uint32_t tex = 0;
    a_glGenTextures(1, &tex);
    a_glBindTexture(GL_TEXTURE_2D, tex);
    while (a_glGetError() != GL_NO_ERROR) {}
    a_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    uint32_t e = a_glGetError();
    if (e != GL_NO_ERROR) {
        sb(s, "    slice %d: glEGLImageTargetTexture2DOES -> GL error 0x%x", slice, e);
        a_eglDestroyImageKHR(g_a_dpy, img);
        return 3;
    }
    a_glBindFramebuffer(GL_FRAMEBUFFER, g_a_fbo);
    a_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    uint32_t st = a_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        sb(s, "    slice %d: FBO INCOMPLETE (0x%x)", slice, st);
        a_eglDestroyImageKHR(g_a_dpy, img);
        return 4;
    }
    a_glViewport(0, 0, w, h);
    a_glClearColor(0.f, 0.f, 1.f, 1.f);      // blue first, then overdrawn, so a
    a_glClear(GL_COLOR_BUFFER_BIT);          // stale clear cannot pass as a draw
    int drew = angle_draw_tri(r, g, b);
    a_glFinish();
    if (!drew) { sb(s, "    slice %d: draw FAILED", slice);
                 a_eglDestroyImageKHR(g_a_dpy, img); return 5; }

    if (!f16) {          // GL's own view of what it just wrote
        uint8_t px[4] = { 0 };
        a_glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int want_r = (int)(r * 255.f + 0.5f), want_g = (int)(g * 255.f + 0.5f);
        sb(s, "    slice %d: GL-side readback = %u,%u,%u  %s", slice, px[0], px[1], px[2],
           (px[0] == want_r && px[1] == want_g) ? "[matches]" : "[MISMATCH]");
    }
    sb(s, "    slice %d: EGLImage -> GL texture -> FBO complete -> draw OK "
          "(%dx%d %s)", slice, w, h, f16 ? "RGBA16F" : "RGBA8");
    a_eglDestroyImageKHR(g_a_dpy, img);
    return 0;
}

// Swift appends its Metal-side verdicts through here, so the whole P13 report
// stays one block in the log rather than two interleaved ones.
void klepton_angle_note(const char *line) {
    if (!g_alog.p) sb_init(&g_alog);
    sb(&g_alog, "%s", line);
}
