// Pre-built stub cells — the named-stub pool, without runtime code generation.
//
// WHY THIS EXISTS
//
// kl_image.c used to mmap 64 KB, write A64 instructions into it and let the
// guest branch there: one 32-byte cell per named stub, carrying the name and
// the handler as literals right after the code. That works on macOS and is a
// SIGKILL on visionOS. Measured on device, 2026-08-07:
//
//     EXC_BAD_ACCESS (SIGKILL), KERN_PROTECTION_FAILURE at 0x10cfa8340
//     termination: namespace CODESIGNING, indicator "Invalid Page"
//     [klepton] stub pool rx 0x10cfa8000..0x10cfb8000
//
// — the faulting pc is the pool, base + 0x340. AMFI does not care that we own
// the pages or that they were mapped RX: code that did not come from a signed
// file cannot be executed, full stop. Note what this is NOT: it is not the
// W^X/RWX question §12.5 answered (mapping RWX is permitted, MAP_JIT is not),
// and it is not M1b. The guest's own libraries are signed dylibs and run fine;
// this is the last hand-written code generation left in the process.
//
// THE SHAPE OF THE FIX
//
// A cell varies from its neighbours only in two 8-byte literals — a payload and
// a target. Those are *data*. The instructions are identical in every cell, so
// they can be built now, signed with the binary, and never written at runtime:
// the cell carries only its own index, and a shared dispatcher reads the pair
// out of a writable table (kl_stub_slots, in kl_image.c) that the allocator
// fills in. Same dispatch, same registers, same cost of one extra branch.
//
// This replaces the mmap path on EVERY platform rather than behind an #ifdef.
// The host is where `make check` runs, and a device-only path would be a path
// nothing tests until it is too late to debug cheaply — the same argument that
// moved the x18 veneer pass offline into klepton-ld.
//
// REGISTERS
//
// Both dispatchers are reached with the cell index in w16 and must respect the
// argument registers, because the guest is mid-call: these sit between the
// caller and a real function and x0-x7 belong to it. x16 and x17 are the
// intra-procedure-call scratch registers, which is exactly what they are
// reserved for, so both are free here.
//
//   named: x0 = payload (the symbol name), branch to target.  Clobbers x0,
//          x16, x17 — deliberately: this shape never returns to the guest, it
//          reports which unimplemented thing was called.
//   trace: x16 = payload (a descriptor), branch to target.  Clobbers x16, x17
//          ONLY — this shape logs and then lets the original call proceed, so
//          every argument register has to arrive at the real function intact.
//
// The old cells clobbered x0/x16 and x16/x17 respectively; x17 is additionally
// used here for the table base, which is free at a call boundary by AAPCS64.

#if defined(__aarch64__)

.text
.align 4

// Two slot arrays, both { void *payload; void *target; } — 16 bytes per entry,
// hence `lsl #4`. Defined in C (kl_image.c) so the allocator can write them.
.globl _kl_stub_named_common
.globl _kl_stub_trace_common

_kl_stub_named_common:
    adrp x17, _kl_stub_named_slots@PAGE
    add  x17, x17, _kl_stub_named_slots@PAGEOFF
    add  x17, x17, x16, lsl #4
    ldr  x0,  [x17]              // payload -> the reporter's first argument
    ldr  x16, [x17, #8]          // target
    br   x16

_kl_stub_trace_common:
    adrp x17, _kl_stub_trace_slots@PAGE
    add  x17, x17, _kl_stub_trace_slots@PAGEOFF
    add  x17, x17, x16, lsl #4
    ldr  x16, [x17]              // payload -> the trampoline's one input
    ldr  x17, [x17, #8]          // target
    br   x17

// The cells themselves. Eight bytes each, and the stride is relied upon by
// kl_image.c (KL_STUB_CELL_BYTES) to turn an index back into an address —
// which is why the two instructions must stay two instructions.
//
// `mov w16, #N` is a single movz for every N below 65536, so the cell count
// can grow to that without changing the shape.

.globl _kl_stub_named_cells
.align 4
_kl_stub_named_cells:
.set cell_i, 0
.rept 16384                      // must equal KL_STUB_NAMED_CELLS in kl_image.c
    mov  w16, #cell_i
    b    _kl_stub_named_common
.set cell_i, cell_i + 1
.endr

.globl _kl_stub_trace_cells
.align 4
_kl_stub_trace_cells:
.set cell_i, 0
.rept 1024
    mov  w16, #cell_i
    b    _kl_stub_trace_common
.set cell_i, cell_i + 1
.endr

#endif  // __aarch64__
