// The Oculus Platform completion-queue gate. `make ovrplat`, in `make check`.
//
// The check lives in kl_ovrplat.c (kl_ovrplat_queue_selftest) because what it
// asserts is that file's own queue and call-surface table, driven through the
// functions the guest's ovr_* calls land in. Testing a copy would test the copy.
//
// No guest, no game files: the requests it issues are the self-test's own
// message types, and a pool's worth of them to reach its limit. stderr
// carries one "[plat] ... completion queued" line per request — over three
// thousand — so it goes to /dev/null; the verdict and any failure are on
// stdout.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_ovrplat.h"

int kl_ovrplat_offline_user_for(const char *target);   // the start window's question

int main(void) {
    if (!freopen("/dev/null", "w", stderr)) return 2;
    printf("=== Oculus Platform completion queue ===\n");
    int ok = kl_ovrplat_queue_selftest(stdout);
    // The offline user is a per-title list: a title on it gets one, a title
    // that is not does not (Shave & Stuff joined it with fix 41).
    unsetenv("KL_PLAT_USER");
    int listed = kl_ovrplat_offline_user_for("x7532973430077887") == 1 &&
                 kl_ovrplat_offline_user_for("x9386827918099701") == 1 &&
                 kl_ovrplat_offline_user_for("ovrplat-test-unlisted") == 0 &&
                 kl_ovrplat_offline_user_for(NULL) == 0;
    printf("%s: the offline user is offered to listed titles only\n", listed ? "PASS" : "FAIL");
    ok = ok && listed;
    // An empty list has no other page: UE asks HasNextPage after GetSize (Into
    // The Radius 2).
    typedef int (*has_page_fn)(const void *);
    has_page_fn next = (has_page_fn)kl_ovrplat_sym("ovr_AchievementProgressArray_HasNextPage");
    has_page_fn prev = (has_page_fn)kl_ovrplat_sym("ovr_LeaderboardEntryArray_HasPreviousPage");
    int paged = next && prev && next(NULL) == 0 && prev(NULL) == 0;
    printf("%s: an empty list has no next or previous page\n", paged ? "PASS" : "FAIL");
    ok = ok && paged;
    printf("%s: no completion lost, no held message rewritten, no slot torn, pool bounded\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
