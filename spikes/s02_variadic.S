.text
.globl _kl_call_aapcs
.p2align 2
// Simulates an AAPCS64 (Linux/Android) variadic call site: the variadic
// argument is passed in a REGISTER (x3). Darwin arm64 expects it on the STACK.
_kl_call_aapcs:                 // (char *buf, size_t n, const char *fmt, long v)
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl  _snprintf               // x0..x3 already loaded, AAPCS64-style
    ldp x29, x30, [sp], #16
    ret
