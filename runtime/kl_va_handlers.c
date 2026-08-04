// C handlers behind the asm thunks in kl_va_thunks.S. Each receives the guest's
// named arguments normally plus an AAPCS64 kl_va*, re-marshals into Darwin layout,
// and calls the host implementation. The host's printf/scanf engine does the
// actual formatting -- we never reimplement it.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "kl_va.h"
#include "klepton.h"

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
int klh_android_log_print(int prio, const char *tag, const char *fmt, kl_va *va) {
    KL_MARSHAL(fmt, va, KL_VA_PRINTF);
    static const char lv[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: ", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "");
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\n', stderr);
    return n;
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
    int fd = open(kl_guest_path(path, kp, sizeof kp), kl_open_flags(flags), (int)kl_va_gp(va));
    kl_fs_trace_open(path, flags, fd);
    return fd;
}
int klh_fcntl(int fd, int cmd, kl_va *va) {
    return fcntl(fd, cmd, (void *)(uintptr_t)kl_va_gp(va));
}
int klh_ioctl(int fd, unsigned long req, kl_va *va) {
    return ioctl(fd, req, (void *)(uintptr_t)kl_va_gp(va));
}
