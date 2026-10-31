// The GPU's working time for this process, and the shaders it had compiled —
// see kl_mtl_busy.m.
#ifndef KL_MTL_BUSY_H
#define KL_MTL_BUSY_H

// Starts counting. Returns 1 when every command buffer committed from here on
// is counted, 0 where there is no Metal device or its command buffers cannot
// be extended. Once per process; later calls return what the first one did.
int kl_mtl_busy_install(void);
// The seconds the GPU worked on this process's command buffers since the last
// call, overlapping spans counted once, and how many buffers that was. Negative
// while nothing is installed.
double kl_mtl_busy_take(unsigned long *buffers);
// How many times the process asked Metal for a library from source or a render
// pipeline state since the last call, in two counts: the requests that took
// long enough to have been compiled, and the ones answered at once from what
// the system had compiled before. Both 0 while nothing is installed.
void kl_mtl_compiles_take(unsigned long *compiled, unsigned long *cached);
// The next render pipeline this thread creates gets this Metal logic operation
// (private API; see kl_mtl_busy.m). -1 for none. Needs kl_mtl_busy_install().
void kl_mtl_logic_op_next(int metal_op);

#endif
