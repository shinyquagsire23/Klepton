// Verifies the AAPCS64 -> Darwin variadic re-marshalling (runtime/kl_va*).
#include <stdio.h>
#include <string.h>

int kl_call_mixed(char *buf, const char *fmt, const char *s);
int kl_call_many(char *buf, const char *fmt);
int kl_call_floats(char *buf, const char *fmt);
int kl_call_scanf(const char *src, const char *fmt, int *a, int *b);

static int fails = 0;
static void check(const char *what, const char *got, const char *want) {
    int ok = strcmp(got, want) == 0;
    printf("  %-28s %-34s %s\n", what, got, ok ? "[OK]" : "[FAIL]");
    if (!ok) { printf("      expected: %s\n", want); fails++; }
}

int main(void) {
    char buf[128];
    printf("=== AAPCS64 -> Darwin variadic thunks ===\n");
    printf("  %-28s %-34s\n", "case", "result");

    memset(buf, 0, sizeof buf);
    kl_call_mixed(buf, "%d %s %.1f %ld", "hi");
    check("mixed int/str/double/long", buf, "42 hi 3.5 99");

    memset(buf, 0, sizeof buf);
    kl_call_many(buf, "%d %d %d %d %d %d %d %d %d");
    check("9 ints (4 spill to stack)", buf, "1 2 3 4 5 6 7 8 9");

    memset(buf, 0, sizeof buf);
    kl_call_floats(buf, "%.1f %.1f %.1f");
    check("3 doubles in v0-v2", buf, "1.5 2.5 3.5");

    int a = 0, b = 0;
    int n = kl_call_scanf("17 4242", "%d %d", &a, &b);
    char got[64];
    snprintf(got, sizeof got, "n=%d a=%d b=%d", n, a, b);
    check("sscanf (args are pointers)", got, "n=2 a=17 b=4242");

    printf("\n%s\n", fails ? "=== FAILURES ===" : "=== all variadic cases pass ===");
    return fails != 0;
}
