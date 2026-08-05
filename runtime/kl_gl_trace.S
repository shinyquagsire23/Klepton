// A per-name GL tracing trampoline — the instrument that names the exact entry
// point the guest called, rather than the neighbourhood a texture census gives.
//
// Why this needs assembly at all. kl_named_stub already builds per-name stubs, but
// it loads the name into x0 and tail-calls a handler: destructive by design, since
// nothing after it needs the arguments. A *tracing* stub is the opposite — it has
// to log the name and then let the real function run with every argument exactly
// as the guest left it. That cannot be expressed in C, because C has no way to
// forward an unknown signature.
//
// The contract, set up by kl_trace_stub() in kl_image.c:
//
//   x16 = &descriptor, where descriptor is { const char *name; void *real; }
//   every other register is the guest's, untouched
//
// x16 and x17 are IP0/IP1, the intra-procedure-call scratch registers. A callee is
// free to clobber them, so no argument can be living there and using them costs
// nothing.
//
// What must survive to the real function:
//   x0-x7   the integer/pointer arguments
//   x8      the indirect result location register (a struct-returning GL call
//           would keep its hidden pointer here; none currently do, but the cost
//           of saving it is one instruction and the cost of missing it is a
//           silently corrupted return)
//   q0-q7   the floating-point arguments — glUniform4f, glClearColor, and every
//           other float entry point lives here, so omitting these would corrupt
//           exactly the calls most likely to be interesting
//   sp      unchanged at the branch, because AAPCS64 stack arguments sit above it.
//           glTexSubImage3D takes 11 arguments and three of them are on the stack;
//           if this frame were still open when the real function ran, it would
//           read our saved registers as its own last three parameters.
//   x30     the guest's return address, so the real function returns to the guest
//           and not into this trampoline
//
// x9-x15 are not saved: they are caller-saved temporaries, and the real function
// is entitled to clobber them anyway.
//
// The tail branch is `br x17`, not `bl`, so this frame is entirely gone by the
// time the real function starts. That keeps the stack shape identical to a direct
// call, which matters for anything that walks frames.

.text
.align 4

.globl _kl_gl_trace_tramp
_kl_gl_trace_tramp:
    // Open a 224-byte frame: 16 for fp/lr, 80 for x0-x8 and x16, 128 for q0-q7.
    stp     x29, x30, [sp, #-224]!
    mov     x29, sp

    stp     x0,  x1,  [sp, #16]
    stp     x2,  x3,  [sp, #32]
    stp     x4,  x5,  [sp, #48]
    stp     x6,  x7,  [sp, #64]
    stp     x8,  x16, [sp, #80]
    stp     q0,  q1,  [sp, #96]
    stp     q2,  q3,  [sp, #128]
    stp     q4,  q5,  [sp, #160]
    stp     q6,  q7,  [sp, #192]

    ldr     x0, [x16]                // descriptor->name
    bl      _kl_gl_trace_log

    ldp     q6,  q7,  [sp, #192]
    ldp     q4,  q5,  [sp, #160]
    ldp     q2,  q3,  [sp, #128]
    ldp     q0,  q1,  [sp, #96]
    ldp     x8,  x16, [sp, #80]
    ldp     x6,  x7,  [sp, #64]
    ldp     x4,  x5,  [sp, #48]
    ldp     x2,  x3,  [sp, #32]
    ldp     x0,  x1,  [sp, #16]

    ldr     x17, [x16, #8]           // descriptor->real
    ldp     x29, x30, [sp], #224     // close the frame BEFORE branching, so the
                                     // real function sees the guest's own sp and
                                     // finds its stack arguments where it left them
    br      x17
