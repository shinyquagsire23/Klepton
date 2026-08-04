// Entry points guest code calls for variadic libc functions.
//
// A guest call arrives AAPCS64-style: named args and the first 8 integer args in
// x0-x7, the first 8 FP args in v0-v7, the rest on the stack. We spill the whole
// register file into a save area, build an AAPCS64 va_list over it, and hand off
// to a C handler that re-marshals into Darwin's layout (see kl_va.c).
//
// Frame:
//   sp+  0 : q0-q7 save area (128B)      vr_top = sp+128
//   sp+128 : x0-x7 save area ( 64B)      gr_top = sp+192
//   sp+192 : kl_va                (32B)
//
// \ngp = number of named (non-variadic) integer parameters.

.text

.macro KL_VTHUNK name, handler, ngp
    .globl _\name
    .p2align 2
_\name:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #224

    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     x0, x1, [sp, #128]
    stp     x2, x3, [sp, #144]
    stp     x4, x5, [sp, #160]
    stp     x6, x7, [sp, #176]

    add     x9,  sp, #192               // &va
    add     x10, x29, #16               // incoming stack args
    str     x10, [x9, #0]               // va.stack
    add     x10, sp, #192
    str     x10, [x9, #8]               // va.gr_top
    add     x10, sp, #128
    str     x10, [x9, #16]              // va.vr_top
    mov     w11, #(-64 + (\ngp * 8))
    str     w11, [x9, #24]              // va.gr_offs
    mov     w11, #-128
    str     w11, [x9, #28]              // va.vr_offs  (no named FP params)

    mov     x\ngp, x9                   // &va becomes the next argument
    bl      _\handler

    add     sp, sp, #224
    ldp     x29, x30, [sp], #16
    ret
.endm

//        guest symbol            C handler              named GP args
KL_VTHUNK klv_printf,             klh_printf,            1
KL_VTHUNK klv_fprintf,            klh_fprintf,           2
KL_VTHUNK klv_sprintf,            klh_sprintf,           2
KL_VTHUNK klv_snprintf,           klh_snprintf,          3
KL_VTHUNK klv_asprintf,           klh_asprintf,          2
KL_VTHUNK klv_dprintf,            klh_dprintf,           2
KL_VTHUNK klv_syslog,             klh_syslog,            2
KL_VTHUNK klv_android_log_print,  klh_android_log_print, 3
KL_VTHUNK klv_sscanf,             klh_sscanf,            2
KL_VTHUNK klv_fscanf,             klh_fscanf,            2
KL_VTHUNK klv_open,               klh_open,              2
KL_VTHUNK klv_fcntl,              klh_fcntl,             2
KL_VTHUNK klv_ioctl,              klh_ioctl,             2

// ---- long double returns ----
// Linux/aarch64 returns `long double` (IEEE binary128) in q0; Darwin returns a
// 64-bit double in d0. Compute in double, then widen into q0 for the guest.
.macro KL_LDTHUNK name, callee
    .globl _\name
    .p2align 2
_\name:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      _\callee              // -> d0
    bl      _kl_f64_to_f128       // d0 -> x0:x1
    fmov    d0, x0
    fmov    v0.d[1], x1
    ldp     x29, x30, [sp], #16
    ret
.endm

KL_LDTHUNK klv_strtold,   strtod
KL_LDTHUNK klv_wcstold,   wcstod
KL_LDTHUNK klv_strtold_l, strtod_l
