// C handlers behind the asm thunks in kl_va_thunks.S. Each receives the guest's
#include <stdint.h>
// named arguments normally plus an AAPCS64 kl_va*, re-marshals into Darwin layout,
// and calls the host implementation. The host's printf/scanf engine does the
// actual formatting -- we never reimplement it.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "kl_va.h"
#include "klepton.h"
#include <errno.h>
#include "../guest/kl_obbmap.h"

// Darwin arm64's va_list is a bare char*, so a marshalled buffer *is* a va_list.
#define KL_MARSHAL(fmt, va, mode)                                        \
    char _m[512] __attribute__((aligned(16)));                           \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, (mode)) == (size_t)-1)  \
        return -1;                                                       \
    va_list _ap = (va_list)_m

int klh_printf(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vprintf(fmt, _ap);
}
int klh_fprintf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vfprintf(kl_host_file(f), fmt, _ap);
}
int klh_sprintf(char *buf, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsprintf(buf, fmt, _ap);
}
int klh_snprintf(char *buf, size_t n, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vsnprintf(buf, n, fmt, _ap);
}
int klh_asprintf(char **out, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vasprintf(out, fmt, _ap);
}
int klh_dprintf(int fd, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    return vdprintf(fd, fmt, _ap);
}
int klh_syslog(int pri, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fprintf(stderr, "[guest syslog %d] ", pri);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
// hl2 (HL2Q3VR): force-call the mod's CreateRenderTargets (libsourcevr 0xec64) on
// g_pSourceVR (libclient global 0xcce4a8), once, the moment VR activates. The engine's
// normal caller is a C++ virtual dispatch we could not trace and it never fires under
// Klepton, so the VR eye render targets (color/depth/gui/menu/scope/intro) are never
// created -> "Eye copy skipped: Source render target is unavailable" -> black. Addresses
// via radare2: g_pSourceVR is stored at libclient+0xcce4a8 right after
// CreateInterface("SourceVirtualReality001"); CreateRenderTargets(0xec64) fetches the
// material system itself, so it takes only `this`. Gated by KL_HL2_FORCERT + target hl2.
// EXPERIMENT: an out-of-context call — if the current thread's GL context is not the
// guest's, or the material system is not ready yet, it may fault. See
// hl2-quest-reference-trace.
// Called from klxr_WaitFrame (kl_openxr.c) — i.e. the guest's render/frame thread with its
// GL context, NOT the log handler (that ran on a worker thread and re-entered logging from
// CreateRenderTargets' own Msg, corrupting the FILE and faulting in fwrite).
void kl_hl2_force_create_rendertargets(void) {
    static int done;
    if (done || !getenv("KL_HL2_FORCERT")) return;
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "hl2") != 0) return;
    extern kl_image *kl_find_image(const char *);
    kl_image *cl = kl_find_image("libclient.so");
    kl_image *sv = kl_find_image("libsourcevr.so");
    kl_image *ms = kl_find_image("libmaterialsystem.so");
    if (!cl || !sv || !ms) { fprintf(stderr, "  [hl2rt] client/sourcevr/matsys not loaded yet\n"); return; }
    void **slot = (void **)((unsigned char *)kl_base(cl) + 0xcce4a8);
    void *sourcevr = *slot;
    if (!sourcevr) { fprintf(stderr, "  [hl2rt] g_pSourceVR NULL — waiting\n"); return; }
    // CreateRenderTargets takes (this, IMaterialSystem*) — matsys is arg1. matsys is the static
    // g_MaterialSystem singleton at libmaterialsystem+0x1b2150 (.bss). Found DETERMINISTICALLY:
    // the InterfaceReg static-init at libmaterialsystem 0xf2884 registers factory 0xe1df0 for
    // "VMaterialSystem081", and that factory is `adrp x0,0x1b2000; add x0,x0,0x150; ret` — i.e.
    // it returns &g_MaterialSystem. This is EXACTLY the object CreateInterface returns and the
    // engine passes to CreateRenderTargets. Its ctor runs at library load, so the vtable is set
    // well before the first frame — no scan (false transient-.data matches), no guest call
    // (fragile factory tail-call / registry .bss read-back-0 / host SIMD strcmp overread), no
    // dependence on libclient wiring timing (its materials globals were still NULL at frame 1).
    extern const char *kl_addr_image(const void *, size_t *);
    size_t off = 0;
    unsigned char *msb = (unsigned char *)kl_base(ms);
    void *matsys = (void *)(msb + 0x1b2150);   // g_MaterialSystem singleton (deterministic)
    // Vtable sanity: a real C++ object's vtable pointer (first word) must land inside its own
    // library's mapping. Verify both objects before we hand control to guest code.
    void *vt_sv = *(void **)sourcevr;
    void *vt_ms = *(void **)matsys;
    const char *own_sv = kl_addr_image(vt_sv, &off);
    const char *own_ms = kl_addr_image(vt_ms, &off);
    fprintf(stderr, "  [hl2rt] sanity: sourcevr=%p vtable=%p in %s | matsys=%p vtable=%p in %s\n",
            sourcevr, vt_sv, own_sv ? own_sv : "(unmapped!)",
            matsys, vt_ms, own_ms ? own_ms : "(unmapped!)");
    if (!own_sv || !strstr(own_sv, "libsourcevr")) {
        fprintf(stderr, "  [hl2rt] ABORT: sourcevr vtable not in libsourcevr — g_pSourceVR wrong\n");
        return;
    }
    if (!own_ms || !strstr(own_ms, "libmaterialsystem")) {
        fprintf(stderr, "  [hl2rt] ABORT: matsys vtable not in libmaterialsystem — CreateInterface wrong\n");
        return;
    }
    // The vtable POINTER being in libmaterialsystem is not enough — the bogus object from the
    // wrong CreateInterface address had exactly that, yet vtable[0x58]=0x232 (garbage) faulted
    // the blr at libsourcevr+0xec18. Validate the actual slot CreateRenderTargets dispatches.
    void *slot58 = *(void **)((unsigned char *)vt_ms + 0x58);
    const char *own_slot = kl_addr_image(slot58, &off);
    fprintf(stderr, "  [hl2rt] sanity: matsys vtable[0x58]=%p in %s\n",
            slot58, own_slot ? own_slot : "(unmapped!)");
    if (!own_slot || !strstr(own_slot, "libmaterialsystem")) {
        fprintf(stderr, "  [hl2rt] ABORT: matsys vtable[0x58] not a real method — wrong interface\n");
        return;
    }
    done = 1;
    // CreateRenderTargets entry = libsourcevr 0xec80 (re-derived for the CURRENT deployed APK —
    // hl2.apk 2026-08-30; earlier 0xebe4/0xec64 were from a STALE build and every forced call ran
    // garbage code, which is what produced the phantom 0x232 crashes and "deadlocks"). AAPCS
    // prologue at 0xec80: sub sp,0x70; stp x29/x30; mov x20,x1=matsys; mov x19,x0=this;
    // bl 0xf058 (cleanup); str x20,[x19,0x2c0]; cbz x20 -> "without material system"; then it
    // dispatches matsys->vtable[0x58]. Found via string xref (retained@0x4744, guard@0x8063).
    void (*fn)(void *, void *) = (void (*)(void *, void *))((unsigned char *)kl_base(sv) + 0xec80);
    unsigned char *svo = (unsigned char *)sourcevr;
    fprintf(stderr, "  [hl2rt] sourcevr fields: 2c0(matsys)=%p 2c8=%p 2d0=%p 2d8=%p 2e0=%p\n",
            *(void **)(svo + 0x2c0), *(void **)(svo + 0x2c8),
            *(void **)(svo + 0x2d0), *(void **)(svo + 0x2d8), *(void **)(svo + 0x2e0));
    // The RTs come back NULL unless the material system is inside a BeginRenderTargetAllocation/
    // EndRenderTargetAllocation block — CreateNamedRenderTargetTextureEx checks a byte flag at
    // matsys+0x3078 (libmaterialsystem 0xeb938: mov w8,0x3078; ldrb w8,[this,x8]; cbz -> "Tried
    // to create render target outside of ... block"). The engine brackets its CreateRenderTargets
    // call with matsys->Begin/EndRenderTargetAllocation(); we set the flag those toggle.
    unsigned char *alloc_flag = (unsigned char *)matsys + 0x3078;
    unsigned char saved_flag = *alloc_flag;
    *alloc_flag = 1;
    fprintf(stderr, "  [hl2rt] force CreateRenderTargets(sourcevr=%p, matsys=%p) fn=%p alloc_flag %d->1\n",
            sourcevr, matsys, (void *)fn, saved_flag);
    fn(sourcevr, matsys);
    *alloc_flag = saved_flag;
    fprintf(stderr, "  [hl2rt] CreateRenderTargets returned (alloc_flag restored to %d)\n", saved_flag);
}

int klh_android_log_print(int prio, const char *tag, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    static const char lv[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: ", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "");
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    // hl2 eye-copy RT probe: the guest logs "began eye submission" on the eye-submission
    // thread right before it reads its render target and (currently) reports it
    // unavailable. Sample the real ANGLE FBO state here — a reliable trigger, since the
    // eye-copy's own glGetIntegerv bypasses the klfb wrapper. See kl_glfb_probe_current_rt.
    if (fmt && (strstr(fmt, "began eye submission") ||
                strstr(fmt, "render target is unavailable"))) {
        extern void kl_glfb_probe_current_rt(const char *);
        kl_glfb_probe_current_rt(strstr(fmt, "unavailable") ? "at 'RT unavailable'"
                                                            : "at 'began eye submission'");
    }
    return n;
}
// BSD err.h, which bionic has and the generated table cannot reach: Darwin
// declares warnx in <err.h>, kl_shim.c does not include it, and the generator's
// answer to "does Darwin declare this" is therefore no. It would be a variadic
// anyway, so a direct forward was never available (AAPCS64 registers vs
// Darwin's stack slots).
//
// The real warnx prefixes the program name and appends a newline, and does NOT
// append strerror the way warn does. OpenJK uses it for its own diagnostics, so
// the prefix says which side of the shim the line came from rather than
// restating a host program name the guest never chose.
int klh_warnx(const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    fputs("[guest warnx] ", stderr);
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
}
// Wide printf. The marshaller reads a narrow format; the conversion specs it
// needs are ASCII, so a byte-per-wchar copy of the format describes the same
// argument list (%s / %ls are both one pointer). Darwin's vswprintf then does
// the formatting. Tea For God (x6323807174332876) builds its strings this way;
// the old stub returned "" for every one of them.
static int kl_wfmt_narrow(const wchar_t *f, char *out, size_t cap) {
    size_t i = 0;
    for (; f && f[i]; i++) {
        if (i + 1 >= cap) return -1;
        out[i] = (f[i] > 0 && f[i] < 0x80) ? (char)f[i] : '?';
    }
    out[i] = 0;
    return 0;
}
int kl_vswprintf_g(wchar_t *s, size_t n, const wchar_t *f, kl_va *va) {
    char nf[1024];
    if (!f || kl_wfmt_narrow(f, nf, sizeof nf) != 0) { if (s && n) s[0] = 0; return -1; }
    KL_MARSHAL(nf, va, KL_VA_PRINTF);
    return vswprintf(s, n, f, _ap);
}
int klh_swprintf(wchar_t *s, size_t n, const wchar_t *f, kl_va *va) {
    return kl_vswprintf_g(s, n, f, va);
}
int klh_sscanf(const char *s, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vsscanf(s, fmt, _ap);
}
int klh_fscanf(void *f, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_SCANF);
    return vfscanf(kl_host_file(f), fmt, _ap);
}

// ---- non-printf variadics: exactly one trailing argument, no format string ----
int kl_open_flags(int lx);          // kl_libc.c
int klh_open(const char *path, int flags, kl_va *va) {
    // Guest passes Linux O_* values, which differ from Darwin's; translate.
    // The mode argument only matters with O_CREAT, but reading it is harmless.
    // The path goes through the /proc rewrite for the same reason fopen does.
    char kp[1024];
    // Xash's con_logfile / -log open their .log via open() with a RELATIVE name
    // against a non-writable CWD, so it fails and the whole server console (incl.
    // scripted_sequence diagnostics) is lost. Rebase a .log WRITE to the writable
    // basedir so kl_xash_tail_enginelog streams it. (Mirrors the fopen path in
    // kl_libc.c.) 3 = O_ACCMODE in the guest's Linux numbering; nonzero = write.
    const char *xbase = getenv("XASH3D_BASEDIR");
    if (xbase && *xbase && path && (flags & 3) != 0) {
        size_t n = strlen(path);
        if (n >= 4 && !strcmp(path + n - 4, ".log")) {
            const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
            char rp[1100];
            snprintf(rp, sizeof rp, "%s/%s", xbase, bn);
            int lfd = open(rp, kl_open_flags(flags), (int)kl_va_gp(va));
            if (lfd >= 0) {
                static int said;
                if (!said++) fprintf(stderr, "  [xash] log open %s -> %s\n", path, rp);
                kl_fs_trace_open(path, flags, lfd);
                return lfd;
            }
        }
    }
    const char *gp = kl_guest_path(path, kp, sizeof kp);
    int fd = open(gp, kl_open_flags(flags), (int)kl_va_gp(va));
    // The OBB read-through (paks, the synthetic .uproject) was reachable only
    // through __open_2 / openat — the doors FORTIFY-built UE4 uses. Older UE4
    // (gnustl era, e.g. Tales from the Galaxy's Edge) calls plain open(), so
    // stat() said Taco.uproject and the paks existed while open() said ENOENT
    // and the engine stopped at "Failed to open descriptor file". Consulted
    // only when the real file is absent, so every open that worked before is
    // unchanged; kl_obbmap_open is a no-op for paths it does not index.
    if (fd < 0 && errno == ENOENT) {
        int vfd = kl_obbmap_open(gp, kl_open_flags(flags));
        if (vfd >= 0) fd = vfd; else errno = ENOENT;
    }
    kl_fs_trace_open(path, flags, fd);
    return fd;
}
// cmd/req and the trailing argument are TRANSLATED, never forwarded raw: the
// two numberings are close enough that a wrong one succeeds meaning something
// else. F_SETFL takes the
// same flag word open() takes — Linux O_NONBLOCK is Darwin O_EXCL — so the
// translation lives next to kl_open_flags in kl_libc.c rather than here.
int kl_fcntl(int fd, int cmd, uintptr_t arg);         // kl_libc.c
int kl_ioctl(int fd, unsigned long req, uintptr_t arg);
int klh_fcntl(int fd, int cmd, kl_va *va) {
    return kl_fcntl(fd, cmd, (uintptr_t)kl_va_gp(va));
}
int klh_ioctl(int fd, unsigned long req, kl_va *va) {
    return kl_ioctl(fd, req, (uintptr_t)kl_va_gp(va));
}
