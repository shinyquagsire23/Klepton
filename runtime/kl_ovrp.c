// libklepton_ovrp — the OVRPlugin replacement. See kl_ovrp.h for why it is a
// replacement rather than a translation.
//
// This is the measurement stage. Unity's C# side reaches OVRPlugin through
// [DllImport("OVRPlugin")], which IL2CPP resolves with dlsym at runtime, so
// serving the soname and naming every lookup gives the real surface without
// guessing at any of the 466 exported entry points.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_ovrp.h"

#define KL_OVRP_MAX 512
static struct { const char *name; unsigned calls; } g_ovrp[KL_OVRP_MAX];
static unsigned g_novrp;

static int ovrp_slot(const char *name) {
    for (unsigned i = 0; i < g_novrp; i++)
        if (strcmp(g_ovrp[i].name, name) == 0) return (int)i;
    if (g_novrp >= KL_OVRP_MAX) return -1;
    g_ovrp[g_novrp].name = strdup(name);
    g_ovrp[g_novrp].calls = 0;
    return (int)g_novrp++;
}

static int g_permissive = -1;
static int permissive(void) {
    if (g_permissive < 0) g_permissive = getenv("KL_PERMISSIVE") != NULL;
    return g_permissive;
}

// Reached through a per-name trampoline, so x0 is the entry point's own name.
// The stub tail-calls here, so this return value is the ovrp_ call's return
// value. ovrpSuccess is 0, which makes a permissive zero mean "it worked" —
// wrong in the usual way, but it is what collects the whole surface in one run.
static uint64_t klovrp_called(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    if (permissive()) {
        if (s >= 0 && g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] call (permissive, returning 0): %s\n", name);
        return 0;
    }
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented OVRPlugin entry "
                    "point '%s'\n", name);
    kl_ovrp_report(stderr);
    kl_fatal_prepare();
    abort();
}

// Unity calls this on every native plugin it loads, handing over its
// IUnityInterfaces registry. The real OVRPlugin uses it to grab
// IUnityGraphicsVulkan/GLES; we record it and do nothing, which is correct until
// there is a renderer to bind to (PLANNING M5/M6).
static void klovrp_UnityPluginLoad(void *unity_interfaces) {
    fprintf(stderr, "  [ovrp] UnityPluginLoad(%p) — recorded; no graphics device "
                    "bound yet\n", unity_interfaces);
}
static void klovrp_UnityPluginUnload(void) {}

static const char g_ovrp_handle[] = "klepton-ovrplugin";

void *kl_ovrp_dlopen(const char *soname) {
    if (!soname) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    // Both spellings occur: Unity asks the ClassLoader for "OVRPlugin" and then
    // dlopens whatever path came back.
    if (strcmp(b, "libOVRPlugin.so") != 0 && strcmp(b, "OVRPlugin") != 0) return NULL;
    fprintf(stderr, "  [ovrp] guest dlopen(\"%s\") -> synthetic OVRPlugin "
                    "(the real one NEEDs libvrapi.so; see PLANNING 3.1)\n", b);
    return (void *)g_ovrp_handle;
}

int kl_ovrp_is_handle(const void *h) { return h == (const void *)g_ovrp_handle; }

static const struct { const char *name; void *fn; } g_ovrp_impl[] = {
    {"UnityPluginLoad",   (void *)klovrp_UnityPluginLoad},
    {"UnityPluginUnload", (void *)klovrp_UnityPluginUnload},
};

void *kl_ovrp_sym(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_ovrp_impl / sizeof g_ovrp_impl[0]; i++)
        if (strcmp(g_ovrp_impl[i].name, name) == 0) return g_ovrp_impl[i].fn;
    ovrp_slot(name);
    return kl_named_stub(name, (void *)klovrp_called);
}

void kl_ovrp_report(FILE *f) {
    static int done;
    if (done || !g_novrp) return;
    done = 1;
    unsigned called = 0;
    for (unsigned i = 0; i < g_novrp; i++) if (g_ovrp[i].calls) called++;
    fprintf(f, "\n=== OVRPlugin surface (M6 work list) ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_novrp, called);
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (g_ovrp[i].calls) fprintf(f, "    %-44s x%u\n", g_ovrp[i].name, g_ovrp[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (!g_ovrp[i].calls) fprintf(f, "    %s\n", g_ovrp[i].name);
}
