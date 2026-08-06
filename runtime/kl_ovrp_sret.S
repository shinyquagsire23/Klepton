// sret capture for kl_ovrp.c — entry thunks for the OVRPlugin entry points
// that return a struct by value. Their callers put the destination pointer in
// x8 (the AAPCS64 indirect-result register), and that value must be sampled
// before ANY call, because every call clobbers x8. Clang demonstrably
// schedules an inline-asm read of x8 past a later call in the same function
// (the memset then lands wherever ovrp_slot left x8 — observed as a strcmp
// crash one frame later), so the read has to happen here, in assembly, at
// the very top. Each thunk moves x8 into the first argument register the C
// signature does not use and tail-branches to the C implementation:
//
//   klovrp_GetNodePoseState_impl(step=w0, nodeId=w1, out=x2)
//   klovrp_GetControllerState2_impl(mask=w0, out=x1)
//   klovrp_GetControllerHapticsDesc_impl(mask=w0, out=x1)

    .text
    .globl  _klovrp_GetNodePoseState_entry
_klovrp_GetNodePoseState_entry:
    mov     x2, x8
    b       _klovrp_GetNodePoseState_impl

    .globl  _klovrp_GetControllerState2_entry
_klovrp_GetControllerState2_entry:
    mov     x1, x8
    b       _klovrp_GetControllerState2_impl

    .globl  _klovrp_GetControllerHapticsDesc_entry
_klovrp_GetControllerHapticsDesc_entry:
    mov     x1, x8
    b       _klovrp_GetControllerHapticsDesc_impl

    .globl  _klovrp_GetControllerState_entry
_klovrp_GetControllerState_entry:
    mov     x1, x8
    b       _klovrp_GetControllerState_impl
