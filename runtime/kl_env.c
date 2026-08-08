//
// Environment var helper functions
//
#include "kl_env.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

int kl_env_on(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return !(!*v || !strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "n")
             || !strcasecmp(v, "off") || !strcasecmp(v, "false"));
}

char* kl_env_str(const char *name, const char *dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return v;
}

int kl_env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return strtol(v, NULL, 0);
}

unsigned int kl_env_uint(const char *name, unsigned int dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return strtoul(v, NULL, 0);
}


float kl_env_float(const char *name, float dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return atof(v);
}

static int g_permissive = -1;
int kl_permissive(void) {
    if (g_permissive < 0) g_permissive = getenv("KL_PERMISSIVE") != NULL;
    return g_permissive;
}