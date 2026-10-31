// The enqueued-panel lifecycle under KL_OVRP_ENQ_RETIRE (Ghost Town, from
// community PR #41). `make ovrplayers`, in `make check`.
//
// Ghost Town's fade quads stayed published after the game had destroyed them,
// a white sheet over the picture. With the switch on, a panel the guest
// destroys (DestroyLayer, or EnqueueDestroyLayer with the id behind a pointer)
// or stops submitting is taken down from the visible set and from the batch
// being gathered, and a later submit of a retired id is not published again.
// The checks live in kl_ovrp.c (kl_ovrp_layer_lifecycle_selftest) because they
// read that file's own batch state.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_ovrp.h"

int main(void) {
    setenv("KL_OVRP_ENQ_RETIRE", "1", 1);
    setenv("KL_OVRP_ENQ_OWNS_OVERLAYS", "1", 1);
    printf("=== enqueued panels: destroy, expiry and the pointer form retire a layer ===\n");
    int ok = kl_ovrp_layer_lifecycle_selftest();
    printf("%s\n", ok ? "PASS layer lifecycle" : "FAIL layer lifecycle");
    return ok ? 0 : 1;
}
