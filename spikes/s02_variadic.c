#include <stdio.h>
#include <string.h>
extern int kl_call_aapcs(char *buf, size_t n, const char *fmt, long v);
int main(void) {
    char a[64] = {0}, b[64] = {0};
    snprintf(a, sizeof a, "value=%ld", 12345L);      // native Darwin call
    kl_call_aapcs(b, sizeof b, "value=%ld", 12345L); // guest-style AAPCS64 call
    printf("  Darwin-ABI call  : \"%s\"\n", a);
    printf("  AAPCS64-ABI call : \"%s\"\n", b);
    printf("\n  VERDICT: %s\n", strcmp(a, b) == 0
        ? "identical -> variadics pass through safely"
        : "DIFFERENT -> variadic thunks are REQUIRED");
    return 0;
}
