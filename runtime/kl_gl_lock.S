// A per-name *locking* GL trampoline, built on the same contract as
// kl_gl_trace.S: route every GL call through a process-wide lock, so that no
// two guest threads are ever inside ANGLE at once. It exists to test one
// hypothesis: the transient INVALID_FRAMEBUFFER_OPERATION the guest's draws
// hit under KL_GLFB_SHARED (framebuffer COMPLETE immediately after, clean
// under lldb, clean standalone) is Bug 1 — ANGLE's shared-context thread
// safety — at draw granularity rather than at compile+link, which is as far
// as the kl_glfb.c lock goes. KL_GLFB_LOCK=1 turns it on; if the frame then
// renders, that is the answer.
//
// The contract is kl_trace_stub()'s, identical to the trace trampoline:
//
//   x16 = &descriptor, where descriptor is { const char *name; void *real; }
//   every other register is the guest's, untouched
//
// Unlike the trace trampoline this cannot tail-branch: the lock must be HELD
// ACROSS the real call, so the real function has to return here. The return
// path is the interesting part:
//
//   - x30 dies at every `bl`, and there are two `bl`s around the real call,
//     so the guest's return address is handed to kl_gl_lock_acquire (x0),
//     which parks it on a per-thread stack in C, and kl_gl_lock_release hands
//     it back in x0. Using a register instead (x19, the first attempt) costs
//     the guest its own callee-saved value — the guest's caller expects x19
//     preserved across the GL call, and writing through the stale ra it left
//     there presented as SIGBUS in guest code, not here.
//   - after the call, x0/x1/q0/q1/x8 are saved across the unlock call: they
//     carry the real function's return value (scalar, small struct, float).
//   - sp is restored to the guest's own before `blr`, exactly as in the trace
//     trampoline, because AAPCS64 stack arguments sit above it.
//
// q2-q7 are not re-saved across the unlock: AAPCS64 returns vector results in
// q0-q3 for short vectors, and GL returns at most one float/double. x2-x7 are
// not return registers.

.text
.align 4

.globl _kl_gl_lock_tramp
_kl_gl_lock_tramp:
    // Open a 224-byte frame, same layout as the trace trampoline.
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

    ldr     x0, [x29, #8]            // guest's return address
    bl      _kl_gl_lock_acquire      // parks it per-thread and takes the lock

    ldp     q6,  q7,  [sp, #192]
    ldp     q4,  q5,  [sp, #160]
    ldp     q2,  q3,  [sp, #128]
    ldp     q0,  q1,  [sp, #96]
    ldp     x8,  x16, [sp, #80]      // the C call killed x16; this brings the
                                     // descriptor pointer back
    ldp     x6,  x7,  [sp, #64]
    ldp     x4,  x5,  [sp, #48]
    ldp     x2,  x3,  [sp, #32]
    ldp     x0,  x1,  [sp, #16]

    ldr     x17, [x16, #8]           // descriptor->real
    ldp     x29, x30, [sp], #224     // close BEFORE the call — stack arguments
                                     // must sit where the guest left them
    blr     x17

    // Keep the return value(s) alive across the unlock call.
    sub     sp, sp, #80
    stp     x0,  x1,  [sp, #16]
    stp     x8,  xzr, [sp, #32]
    stp     q0,  q1,  [sp, #48]

    bl      _kl_gl_lock_release      // drops the lock, returns the parked ra
    mov     x30, x0

    ldp     x8,  xzr, [sp, #32]
    ldp     q0,  q1,  [sp, #48]
    ldp     x0,  x1,  [sp, #16]
    add     sp, sp, #80

    ret                              // back to the guest, not into ourselves
