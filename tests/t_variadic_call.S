// Synthetic guest call sites: arguments placed exactly as an AAPCS64 (Android)
// compiler would place them, then a direct branch to klepton's variadic thunk.
.text

// kl_call_mixed(char *buf, const char *fmt, const char *s)
//   -> snprintf(buf, 64, fmt, 42, s, 99L, 3.5)
//      GP: x3=42  x4=s  x5=99      FP: d0=3.5
.globl _kl_call_mixed
.p2align 2
_kl_call_mixed:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x9,  x1                 // fmt
    mov     x10, x2                 // s
    mov     x1,  #64
    mov     x2,  x9
    mov     x3,  #42
    mov     x4,  x10
    mov     x5,  #99
    fmov    d0,  #3.5
    bl      _klv_snprintf
    ldp     x29, x30, [sp], #16
    ret

// kl_call_many(char *buf, const char *fmt)
//   -> snprintf(buf, 64, fmt, 1..9)  -- 6..9 spill to the stack, exercising the
//      overflow path where gr_offs has run out.
.globl _kl_call_many
.p2align 2
_kl_call_many:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x9, x1
    sub     sp, sp, #32
    mov     x10, #6
    str     x10, [sp, #0]
    mov     x10, #7
    str     x10, [sp, #8]
    mov     x10, #8
    str     x10, [sp, #16]
    mov     x10, #9
    str     x10, [sp, #24]
    mov     x1, #64
    mov     x2, x9
    mov     x3, #1
    mov     x4, #2
    mov     x5, #3
    mov     x6, #4
    mov     x7, #5
    bl      _klv_snprintf
    add     sp, sp, #32
    ldp     x29, x30, [sp], #16
    ret

// kl_call_floats(char *buf, const char *fmt) -> snprintf(buf,64,fmt, 1.5,2.5,3.5)
.globl _kl_call_floats
.p2align 2
_kl_call_floats:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x9, x1
    mov     x1, #64
    mov     x2, x9
    fmov    d0, #1.5
    fmov    d1, #2.5
    fmov    d2, #3.5
    bl      _klv_snprintf
    ldp     x29, x30, [sp], #16
    ret

// kl_call_scanf(const char *src, const char *fmt, int *a, int *b)
//   -> sscanf(src, fmt, a, b)   GP: x2=a x3=b  (scanf args are all pointers)
.globl _kl_call_scanf
.p2align 2
_kl_call_scanf:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      _klv_sscanf
    ldp     x29, x30, [sp], #16
    ret
