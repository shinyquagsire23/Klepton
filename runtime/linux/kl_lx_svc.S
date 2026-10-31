// Where a veneered `svc #0` lands (kl_x18.c emits the veneer, kl_lx.h says what
// the frame is).
//
// The veneer has pushed the guest's x16 and x30 and called here through x16, so
// on entry sp is 16 below the guest's and x30 returns into the veneer. Linux
// preserves every register but x0 across a syscall — NZCV and the vector
// registers too — and glibc's inline syscalls declare no clobbers at all, so
// nothing the C handler is allowed to trash may leak back.
//
// This runs on the guest's stack. That is a choice, not an oversight: the
// handler's own frames are small, and a guest thread that cannot spare them
// could not have made the call on Linux either, where signal delivery needs
// more.
#define FRAME 768

.text
.align 2
.globl _kl_lx_svc_entry
_kl_lx_svc_entry:
    sub  sp, sp, #FRAME
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30,      [sp, #240]
    mrs  x9, nzcv
    str  x9,       [sp, #248]
    stp  q0,  q1,  [sp, #256]
    stp  q2,  q3,  [sp, #288]
    stp  q4,  q5,  [sp, #320]
    stp  q6,  q7,  [sp, #352]
    stp  q8,  q9,  [sp, #384]
    stp  q10, q11, [sp, #416]
    stp  q12, q13, [sp, #448]
    stp  q14, q15, [sp, #480]
    stp  q16, q17, [sp, #512]
    stp  q18, q19, [sp, #544]
    stp  q20, q21, [sp, #576]
    stp  q22, q23, [sp, #608]
    stp  q24, q25, [sp, #640]
    stp  q26, q27, [sp, #672]
    stp  q28, q29, [sp, #704]
    stp  q30, q31, [sp, #736]

    mov  x0, sp
    bl   _kl_lx_syscall
    str  x0, [sp, #0]

    ldp  q0,  q1,  [sp, #256]
    ldp  q2,  q3,  [sp, #288]
    ldp  q4,  q5,  [sp, #320]
    ldp  q6,  q7,  [sp, #352]
    ldp  q8,  q9,  [sp, #384]
    ldp  q10, q11, [sp, #416]
    ldp  q12, q13, [sp, #448]
    ldp  q14, q15, [sp, #480]
    ldp  q16, q17, [sp, #512]
    ldp  q18, q19, [sp, #544]
    ldp  q20, q21, [sp, #576]
    ldp  q22, q23, [sp, #608]
    ldp  q24, q25, [sp, #640]
    ldp  q26, q27, [sp, #672]
    ldp  q28, q29, [sp, #704]
    ldp  q30, q31, [sp, #736]
    ldr  x9,       [sp, #248]
    msr  nzcv, x9
    ldp  x0,  x1,  [sp, #0]
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    ldr  x30,      [sp, #240]
    add  sp, sp, #FRAME
    ret

// kl_lx_enter(entry, sp): hand a new process to its entry point. The kernel
// gives it nothing but a stack — every register it could mistake for an argument
// is zero, x0 in particular, which a dynamic loader's entry reads as "no
// finaliser registered".
.globl _kl_lx_enter
_kl_lx_enter:
    mov  x16, x0
    mov  sp,  x1
    mov  x0,  #0
    mov  x1,  #0
    mov  x2,  #0
    mov  x3,  #0
    mov  x4,  #0
    mov  x5,  #0
    mov  x6,  #0
    mov  x7,  #0
    mov  x8,  #0
    mov  x29, #0
    mov  x30, #0
    br   x16

// kl_lx_thread_resume(frame, sp): continue after an `svc` from a saved frame, on
// a given stack. A cloned thread starts this way — the frame is its parent's,
// with x[0] set to clone's answer in the child — and so does a vfork parent when
// its child is done. Every register comes from the frame. The veneer the parent came through has
// still to run its second half — pop x16 and x30, branch back — so the pair it
// pushed is planted below the child's stack pointer for it to find, and x16
// (which that pop overwrites) is free to hold the frame's address on the way.
#define FRAME_PAIR 768

.globl _kl_lx_thread_resume
_kl_lx_thread_resume:
    mov  x16, x0
    sub  x1, x1, #16
    add  x2, x16, #FRAME_PAIR
    ldp  x2, x3, [x2]
    stp  x2, x3, [x1]
    mov  sp, x1
    ldp  q0,  q1,  [x16, #256]
    ldp  q2,  q3,  [x16, #288]
    ldp  q4,  q5,  [x16, #320]
    ldp  q6,  q7,  [x16, #352]
    ldp  q8,  q9,  [x16, #384]
    ldp  q10, q11, [x16, #416]
    ldp  q12, q13, [x16, #448]
    ldp  q14, q15, [x16, #480]
    ldp  q16, q17, [x16, #512]
    ldp  q18, q19, [x16, #544]
    ldp  q20, q21, [x16, #576]
    ldp  q22, q23, [x16, #608]
    ldp  q24, q25, [x16, #640]
    ldp  q26, q27, [x16, #672]
    ldp  q28, q29, [x16, #704]
    ldp  q30, q31, [x16, #736]
    ldr  x9,       [x16, #248]
    msr  nzcv, x9
    ldr  x1,       [x16, #8]
    ldp  x2,  x3,  [x16, #16]
    ldp  x4,  x5,  [x16, #32]
    ldp  x6,  x7,  [x16, #48]
    ldp  x8,  x9,  [x16, #64]
    ldp  x10, x11, [x16, #80]
    ldp  x12, x13, [x16, #96]
    ldp  x14, x15, [x16, #112]
    ldr  x17,      [x16, #136]
    ldp  x18, x19, [x16, #144]
    ldp  x20, x21, [x16, #160]
    ldp  x22, x23, [x16, #176]
    ldp  x24, x25, [x16, #192]
    ldp  x26, x27, [x16, #208]
    ldp  x28, x29, [x16, #224]
    ldr  x30,      [x16, #240]
    ldr  x0,       [x16, #0]
    ret
