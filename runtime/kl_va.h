// AAPCS64 (Linux/Android) varargs -> Darwin arm64 varargs.
//
// Guest variadic call sites pass arguments in x0-x7 / v0-v7 per AAPCS64.
// Darwin arm64 passes variadic arguments on the STACK, and its va_list is a bare
// char* rather than AAPCS64's 32-byte descriptor. Neither the call nor a guest
// va_list can be forwarded; both must be re-marshalled. Proven in spikes/s02.
#ifndef KL_VA_H
#define KL_VA_H
#include <stddef.h>
#include <stdint.h>

// AAPCS64 va_list, as materialised by the asm thunks in kl_va_thunks.S.
typedef struct {
    void *stack;        // next stack-passed argument
    void *gr_top;       // one past the end of the x0-x7 save area
    void *vr_top;       // one past the end of the q0-q7 save area
    int32_t gr_offs;    // negative byte offset from gr_top to the next GP arg
    int32_t vr_offs;    // negative byte offset from vr_top to the next FP arg
} kl_va;

// Pull one argument of the given class out of an AAPCS64 va_list.
uint64_t kl_va_gp(kl_va *v);     // integer / pointer
double   kl_va_fp(kl_va *v);     // double (floats are promoted)

// Walk `fmt`, pull each argument from `src`, and write them into `dst` using
// Darwin's stack-argument layout. Returns bytes written, or (size_t)-1 if `dst`
// is too small. `dst` must be 16-byte aligned.
//
// mode KL_VA_PRINTF: %d consumes an int (4 bytes), %f a double, %s a pointer.
// mode KL_VA_SCANF : every conversion consumes a POINTER (8 bytes), and '*'
//                    means assignment suppression rather than a width argument.
enum { KL_VA_PRINTF = 0, KL_VA_SCANF = 1 };
size_t kl_va_marshal(const char *fmt, kl_va *src, void *dst, size_t cap, int mode);

#endif
