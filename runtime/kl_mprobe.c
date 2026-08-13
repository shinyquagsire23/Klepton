#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_mprobe.h"

// ---- the slice of IL2CPP's embedding API this needs ----
// All exported by libil2cpp.so (checked with nm), so they resolve through the
// image registry like any other guest symbol.
typedef struct Il2CppDomain   Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage    Il2CppImage;
typedef struct Il2CppClass    Il2CppClass;
typedef struct MethodInfo     MethodInfo;
typedef struct Il2CppObject   Il2CppObject;

static struct {
    Il2CppDomain     *(*domain_get)(void);
    const Il2CppAssembly **(*domain_get_assemblies)(const Il2CppDomain *, size_t *);
    const Il2CppImage *(*assembly_get_image)(const Il2CppAssembly *);
    Il2CppClass      *(*class_from_name)(const Il2CppImage *, const char *, const char *);
    const MethodInfo *(*class_get_method_from_name)(Il2CppClass *, const char *, int);
    Il2CppObject     *(*runtime_invoke)(const MethodInfo *, void *, void **, Il2CppObject **);
    void             *(*object_unbox)(Il2CppObject *);
    Il2CppObject     *(*string_new)(const char *);
    uint32_t          (*array_length)(Il2CppObject *);
    Il2CppClass      *(*object_get_class)(Il2CppObject *);
    const char       *(*class_get_name)(Il2CppClass *);
    void             *(*class_get_type)(Il2CppClass *);
    Il2CppObject     *(*type_get_object)(void *);
} il;

// Managed object layouts (v24 / Unity 2019.4, LP64). Only the fields read here.
//   Il2CppObject { Il2CppClass *klass; void *monitor; }              16 B
//   Il2CppString { Il2CppObject; int32_t length; uint16_t chars[]; } chars at 20
//   Il2CppArray  { Il2CppObject; void *bounds; uintptr_t max_length; } data at 32
#define OBJ_HDR   16
#define STR_LEN   16
#define STR_CHARS 20
#define ARR_DATA  32

static int g_state;          // 0 = untried, 1 = up, -1 = unavailable

static int mprobe_init(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return 0;
#define BIND(field, sym) \
    do { *(void **)&il.field = kl_sym(img, sym); \
         if (!il.field) { fprintf(stderr, "  [mprobe] libil2cpp exports no %s\n", sym); \
                          return 0; } } while (0)
    BIND(domain_get,                "il2cpp_domain_get");
    BIND(domain_get_assemblies,     "il2cpp_domain_get_assemblies");
    BIND(assembly_get_image,        "il2cpp_assembly_get_image");
    BIND(class_from_name,           "il2cpp_class_from_name");
    BIND(class_get_method_from_name,"il2cpp_class_get_method_from_name");
    BIND(runtime_invoke,            "il2cpp_runtime_invoke");
    BIND(object_unbox,              "il2cpp_object_unbox");
    BIND(string_new,                "il2cpp_string_new");
    BIND(array_length,              "il2cpp_array_length");
    BIND(object_get_class,          "il2cpp_object_get_class");
    BIND(class_get_name,            "il2cpp_class_get_name");
    BIND(class_get_type,            "il2cpp_class_get_type");
    BIND(type_get_object,           "il2cpp_type_get_object");
#undef BIND
    g_state = 1;
    return 1;
}

// Search every loaded assembly rather than naming one: which module a type
// lives in is a Unity-version detail (Input moved to UnityEngine.CoreModule,
// InputTracking to UnityEngine.XRModule), and 87 images is a cheap scan done
// once per method.
static Il2CppClass *find_class(const char *ns, const char *name) {
    Il2CppDomain *dom = il.domain_get();
    if (!dom) return NULL;
    size_t n = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(dom, &n);
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage *im = il.assembly_get_image(as[i]);
        if (!im) continue;
        Il2CppClass *k = il.class_from_name(im, ns, name);
        if (k) return k;
    }
    return NULL;
}

static const MethodInfo *find_method(const char *ns, const char *cls,
                                     const char *meth, int argc) {
    Il2CppClass *k = find_class(ns, cls);
    if (!k) { fprintf(stderr, "  [mprobe] no class %s.%s\n", ns, cls); return NULL; }
    const MethodInfo *m = il.class_get_method_from_name(k, meth, argc);
    if (!m) fprintf(stderr, "  [mprobe] %s.%s has no %s/%d\n", ns, cls, meth, argc);
    return m;
}

// Invoke, reporting a managed throw by class name instead of letting it
// escape. A probe that aborts the run it is measuring is worse than no probe.
static Il2CppObject *invoke_on(Il2CppObject *self, const MethodInfo *m,
                               void **args, const char *what) {
    if (!m) return NULL;
    Il2CppObject *exc = NULL;
    Il2CppObject *r = il.runtime_invoke(m, self, args, &exc);
    if (exc) {
        Il2CppClass *k = il.object_get_class(exc);
        fprintf(stderr, "  [mprobe] %s threw %s\n", what,
                k ? il.class_get_name(k) : "?");
        return NULL;
    }
    return r;
}

static Il2CppObject *invoke(const MethodInfo *m, void **args, const char *what) {
    return invoke_on(NULL, m, args, what);   // static methods
}

static float invoke_float(const MethodInfo *m, void **args, const char *what) {
    Il2CppObject *r = invoke(m, args, what);
    if (!r) return 0.0f;
    return *(float *)il.object_unbox(r);
}

// Managed string -> ASCII, for the joystick names.
static void str_ascii(Il2CppObject *s, char *out, size_t cap) {
    if (!s) { snprintf(out, cap, "(null)"); return; }
    int32_t len = *(int32_t *)((uint8_t *)s + STR_LEN);
    const uint16_t *ch = (const uint16_t *)((uint8_t *)s + STR_CHARS);
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 1 < cap; i++)
        out[o++] = (ch[i] >= 0x20 && ch[i] < 0x7f) ? (char)ch[i] : '?';
    out[o] = 0;
}

// ---- KL_PROBE_ENUM: read an enum's VALUES out of the running guest --------
//
// The values of a C# enum are the one thing about the guest's ABI that is
// neither in its exported symbols nor in its strings: global-metadata.dat holds
// the names, and the numbers live in a constant table whose layout is a
// metadata-version detail. But the runtime knows them, and it will answer
// through ordinary reflection — `Enum.GetNames(t)` and `Enum.GetValues(t)`.
//
// This exists because kl_ovrplat has to hand the guest a MESSAGE, and a message
// carries `Message.MessageType`, which the guest's own `ParseMessageHandle`
// switches on: a number we guessed lands in its `default` arm, which logs
// "Unrecognized message type" and produces no message at all. Same argument as
// OVRP_HEADSET_OCULUS_QUEST_2 in kl_ovrp.c — read the value out of the guest
// rather than out of a header we do not have — with reflection instead of a
// metadata scan, because that also works for a nested type.
//
//   KL_PROBE_ENUM="Oculus.Platform.Message/MessageType,Oculus.Platform.User..."
//
// Comma-separated, `Namespace.Type` (nested types with `/`), printed once at
// the first probe tick. Diagnostic only; nothing in the shipping path reads it.
static void probe_dump_enum(const char *spec) {
    char ns[128] = "", name[128] = "";
    const char *dot = strrchr(spec, '.');
    if (dot) {
        size_t nl = (size_t)(dot - spec);
        if (nl >= sizeof ns) nl = sizeof ns - 1;
        memcpy(ns, spec, nl); ns[nl] = 0;
        snprintf(name, sizeof name, "%s", dot + 1);
    } else {
        snprintf(name, sizeof name, "%s", spec);   // the global namespace
    }
    Il2CppClass *k = find_class(ns, name);
    if (!k) { fprintf(stderr, "  [mprobe] enum: no class %s.%s\n", ns, name); return; }
    void         *ty  = il.class_get_type(k);
    Il2CppObject *tyo = ty ? il.type_get_object(ty) : NULL;
    if (!tyo) { fprintf(stderr, "  [mprobe] enum: no System.Type for %s\n", spec); return; }

    const MethodInfo *m_names  = find_method("System", "Enum", "GetNames", 1);
    const MethodInfo *m_values = find_method("System", "Enum", "GetValues", 1);
    void *args[1] = { tyo };
    Il2CppObject *names  = invoke(m_names,  args, "Enum.GetNames");
    Il2CppObject *values = invoke(m_values, args, "Enum.GetValues");
    if (!names || !values) return;
    uint32_t n = il.array_length(names);
    // GetValues returns an array OF THE ENUM TYPE, so its elements are the
    // underlying integer, not boxed objects. Four bytes unless the enum has an
    // explicit wider base, which none of the ones this exists for do — the
    // stride is checked against the array length rather than assumed, so a
    // long-backed enum prints as such instead of silently reading halves.
    const uint8_t *vdata = (const uint8_t *)values + ARR_DATA;
    const void   **ndata = (const void **)((const uint8_t *)names + ARR_DATA);
    fprintf(stderr, "  [mprobe] enum %s: %u value(s)\n", spec, n);
    for (uint32_t i = 0; i < n; i++) {
        char nm[128];
        str_ascii((Il2CppObject *)ndata[i], nm, sizeof nm);
        int32_t v; memcpy(&v, vdata + (size_t)i * 4, sizeof v);
        fprintf(stderr, "  [mprobe]   %-56s = %d (0x%08x)\n", nm, v, (unsigned)v);
    }
}

static void probe_dump_enums(void) {
    const char *spec = kl_env_str("KL_PROBE_ENUM", NULL);
    if (!spec || !*spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf, *tok; (tok = strsep(&p, ",")) != NULL; )
        if (*tok) probe_dump_enum(tok);
}

// The axes this title binds (PLANNING M7: parsed out of globalgamemanagers).
static const char *const AXES[] = {
    "TriggerLeftHand", "TriggerRightHand",
    "HorizontalLeftHand", "VerticalLeftHand",
    "HorizontalRightHand", "VerticalRightHand",
    "MenuButtonLeftHand", "MenuButtonRightHand",
};
#define N_AXES (sizeof AXES / sizeof AXES[0])

void kl_mprobe_tick(unsigned frame) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_PROBE_INPUT", 0);
    static unsigned every, from;
    if (!every) {
        every = kl_env_uint("KL_PROBE_EVERY", 120);
        if (!every) every = 120;
        // Probing is not free of side effects: the first call into a managed
        // class runs its cctor, and doing that to OVRInput/OVRPlugin before the
        // guest has loaded the plugin drags the runtime down paths it would not
        // take on its own (one such run aborted on an ovrp entry point the game
        // reaches only from there). Wait for the game to be up.
        from = kl_env_uint("KL_PROBE_FROM", 1200u);
    }
    if (frame < from || frame % every) return;
    if (!mprobe_init()) { on = 0; return; }

    static int dumped_enums;
    if (!dumped_enums) { dumped_enums = 1; probe_dump_enums(); }

    // Input.GetJoystickNames and InputTracking.GetLocalPosition/Rotation are
    // NOT here: IL2CPP's managed stripping removed them, which is itself the
    // measurement — this title reaches poses through OVRInput, below.
    static const MethodInfo *m_axis;
    static const MethodInfo *m_cpos, *m_crot, *m_conn, *m_pvalid, *m_ptracked;
    static Il2CppObject *axis_str[N_AXES];
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_axis  = find_method("UnityEngine", "Input", "GetAxis", 1);
        // The game's own pose seam. VRController::Update asks
        // IVRPlatformHelper::GetNodePose, which lands in OVRInput, which asks
        // OVRNodeStateProperties -> InputTracking::GetNodeStates -> libunity's
        // node table -> our ovrp node poses. Static, so callable from here.
        m_cpos     = find_method("", "OVRInput", "GetLocalControllerPosition", 1);
        m_crot     = find_method("", "OVRInput", "GetLocalControllerRotation", 1);
        m_conn     = find_method("", "OVRInput", "GetConnectedControllers", 0);
        m_pvalid   = find_method("", "OVRInput", "GetControllerPositionValid", 1);
        m_ptracked = find_method("", "OVRInput", "GetControllerPositionTracked", 1);
        for (unsigned i = 0; i < N_AXES; i++) axis_str[i] = il.string_new(AXES[i]);
    }

    // Which VR device does Unity think is loaded? This title ships four
    // IVRPlatformHelper implementations (Oculus, OpenVR, PSVR and
    // Deviceless), and DevicelessVRHelper::GetNodePose reports no pose BY
    // DESIGN — a game running on that helper has a working menu, working
    // Input.GetAxis and no controllers at all, which is exactly the symptom.
    // The choice keys on this surface, and OculusVRHelper also picks its
    // controller-transform offset from the device model.
    static const MethodInfo *m_devname, *m_xrenabled, *m_present, *m_model;
    static int resolved_xr;
    if (!resolved_xr) {
        resolved_xr = 1;
        m_devname   = find_method("UnityEngine.XR", "XRSettings", "get_loadedDeviceName", 0);
        m_xrenabled = find_method("UnityEngine.XR", "XRSettings", "get_enabled", 0);
        m_present   = find_method("UnityEngine.XR", "XRDevice", "get_isPresent", 0);
        m_model     = find_method("UnityEngine.XR", "XRDevice", "get_model", 0);
    }
    // Focus, as MANAGED OVRPlugin computes it. The game acts on these: it has
    // inputFocusWasCaptured/Released events and hides its menu controllers
    // while focus is away. Every one of these getters is a candidate for the
    // ovrpResult-vs-ovrpBool trap that made node validity read false.
    static const MethodInfo *m_infocus, *m_vrfocus, *m_userp;
    static int resolved_focus;
    if (!resolved_focus) {
        resolved_focus = 1;
        m_infocus = find_method("", "OVRPlugin", "get_hasInputFocus", 0);
        m_vrfocus = find_method("", "OVRPlugin", "get_hasVrFocus", 0);
        m_userp   = find_method("", "OVRPlugin", "get_userPresent", 0);
    }
    {
        Il2CppObject *a = invoke(m_infocus, NULL, "OVRPlugin.hasInputFocus");
        Il2CppObject *b = invoke(m_vrfocus, NULL, "OVRPlugin.hasVrFocus");
        Il2CppObject *c2 = invoke(m_userp, NULL, "OVRPlugin.userPresent");
        fprintf(stderr, "  [mprobe f%u] focus: input=%s vr=%s userPresent=%s\n", frame,
                a ? (*(uint8_t *)il.object_unbox(a) ? "1" : "0") : "?",
                b ? (*(uint8_t *)il.object_unbox(b) ? "1" : "0") : "?",
                c2 ? (*(uint8_t *)il.object_unbox(c2) ? "1" : "0") : "?");
    }
    {
        char dn[128] = "?", md[128] = "?";
        Il2CppObject *d = invoke(m_devname, NULL, "XRSettings.loadedDeviceName");
        Il2CppObject *m = invoke(m_model, NULL, "XRDevice.model");
        if (d) str_ascii(d, dn, sizeof dn);
        if (m) str_ascii(m, md, sizeof md);
        Il2CppObject *en = invoke(m_xrenabled, NULL, "XRSettings.enabled");
        Il2CppObject *pr = invoke(m_present, NULL, "XRDevice.isPresent");
        fprintf(stderr, "  [mprobe f%u] xr: device=\"%s\" model=\"%s\" enabled=%s present=%s\n",
                frame, dn, md,
                en ? (*(uint8_t *)il.object_unbox(en) ? "1" : "0") : "?",
                pr ? (*(uint8_t *)il.object_unbox(pr) ? "1" : "0") : "?");
    }

    // The game's own controller objects, read straight out of the live scene.
    // FindObjectsOfType returns only components on ACTIVE objects while
    // Resources.FindObjectsOfTypeAll includes inactive ones, so the pair
    // separates "the controllers were never built" from "they exist and
    // something disabled them" — which is what "no saber hilts render" is
    // asking. VRController::get_active is the game's own verdict on whether
    // its node has a pose.
    // KL_PROBE_TYPES: comma-separated managed types to census, each as
    // Namespace.Type (bare name for the global namespace). The default set is
    // the menu pointer chain.
    static const MethodInfo *m_find, *m_findall, *m_vactive, *m_vnode, *m_vpos;
    static Il2CppObject *vrc_type;
    static Il2CppObject *types[12];
    static char          tnames[12][64];
    static unsigned      n_types;
    static int resolved_scene;
    if (!resolved_scene) {
        resolved_scene = 1;
        m_find    = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_findall = find_method("UnityEngine", "Resources", "FindObjectsOfTypeAll", 1);
        m_vactive = find_method("", "VRController", "get_active", 0);
        m_vnode   = find_method("", "VRController", "get_node", 0);
        m_vpos    = find_method("", "VRController", "get_position", 0);
        Il2CppClass *k = find_class("", "VRController");
        if (k) vrc_type = il.type_get_object(il.class_get_type(k));

        const char *spec = kl_env_str("KL_PROBE_TYPES", "VRUIControls.VRPointer,VRUIControls.VRInputModule,"
                                                        "VRUIControls.VRLaserPointer,VRUIControls.VRGraphicRaycaster");
        char buf[512];
        snprintf(buf, sizeof buf, "%s", spec);
        for (char *tok = strtok(buf, ","); tok && n_types < 12;
             tok = strtok(NULL, ",")) {
            char ns[64] = "", nm[64];
            const char *dot = strrchr(tok, '.');
            if (dot) {
                size_t l = (size_t)(dot - tok);
                if (l >= sizeof ns) l = sizeof ns - 1;
                memcpy(ns, tok, l); ns[l] = 0;
                snprintf(nm, sizeof nm, "%s", dot + 1);
            } else {
                snprintf(nm, sizeof nm, "%s", tok);
            }
            Il2CppClass *kk = find_class(ns, nm);
            if (!kk) continue;
            snprintf(tnames[n_types], sizeof tnames[0], "%s", nm);
            types[n_types++] = il.type_get_object(il.class_get_type(kk));
        }
    }
    if (n_types) {
        fprintf(stderr, "  [mprobe f%u] scene:", frame);
        for (unsigned i = 0; i < n_types; i++) {
            void *ta[1] = { types[i] };
            Il2CppObject *a = invoke(m_find, ta, "FindObjectsOfType");
            Il2CppObject *b = invoke(m_findall, ta, "FindObjectsOfTypeAll");
            fprintf(stderr, " %s=%d/%d", tnames[i],
                    a ? (int)il.array_length(a) : -1,
                    b ? (int)il.array_length(b) : -1);
        }
        fprintf(stderr, "  (active/all)\n");
    }
    // Walk each VRController up its parent chain, naming every object and
    // whether it is switched off in its own right (activeSelf). An object can
    // be inactive because a PARENT is inactive, and only the ancestor that is
    // actually off is the thing to explain — the rest are consequences.
    static const MethodInfo *m_go, *m_tf, *m_parent, *m_aself, *m_ahier, *m_ptr_ctrl;
    static const MethodInfo *m_objname;
    static int resolved_walk;
    if (!resolved_walk) {
        resolved_walk = 1;
        m_objname = find_method("UnityEngine", "Object", "get_name", 0);
        m_go     = find_method("UnityEngine", "Component", "get_gameObject", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_parent = find_method("UnityEngine", "Transform", "get_parent", 0);
        m_aself  = find_method("UnityEngine", "GameObject", "get_activeSelf", 0);
        m_ahier  = find_method("UnityEngine", "GameObject", "get_activeInHierarchy", 0);
        m_ptr_ctrl = find_method("VRUIControls", "VRPointer", "get_vrController", 0);
    }
    // Which controller does the live pointer actually want? A null here means
    // the pointer was never wired to one, which is a different bug from a
    // controller that exists and is switched off.
    if (vrc_type && m_ptr_ctrl && n_types) {
        void *ta[1] = { types[0] };            // VRPointer is first by default
        Il2CppObject *ps = invoke(m_find, ta, "FindObjectsOfType(VRPointer)");
        if (ps && il.array_length(ps)) {
            Il2CppObject *p0 = ((Il2CppObject **)((uint8_t *)ps + ARR_DATA))[0];
            Il2CppObject *vc = invoke_on(p0, m_ptr_ctrl, NULL, "VRPointer.vrController");
            fprintf(stderr, "  [mprobe f%u] VRPointer.vrController=%s\n", frame,
                    vc ? "set" : "<null>");
        }
    }
    if (vrc_type) {
        void *targs[1] = { vrc_type };
        Il2CppObject *act = invoke(m_find, targs, "Object.FindObjectsOfType");
        Il2CppObject *all = invoke(m_findall, targs, "Resources.FindObjectsOfTypeAll");
        fprintf(stderr, "  [mprobe f%u] VRController: active=%d all=%d", frame,
                act ? (int)il.array_length(act) : -1,
                all ? (int)il.array_length(all) : -1);
        // Report each one the game built, however it was found.
        Il2CppObject *list = all ? all : act;
        uint32_t n = list ? il.array_length(list) : 0;
        for (uint32_t i = 0; i < n && i < 4; i++) {
            Il2CppObject *vc = ((Il2CppObject **)((uint8_t *)list + ARR_DATA))[i];
            if (!vc) continue;
            Il2CppObject *a = invoke_on(vc, m_vactive, NULL, "VRController.active");
            Il2CppObject *nd = invoke_on(vc, m_vnode, NULL, "VRController.node");
            Il2CppObject *p = invoke_on(vc, m_vpos, NULL, "VRController.position");
            fprintf(stderr, " | node=%s active=%s",
                    nd ? (*(int32_t *)il.object_unbox(nd) == 4 ? "L" :
                          *(int32_t *)il.object_unbox(nd) == 5 ? "R" : "?") : "?",
                    a ? (*(uint8_t *)il.object_unbox(a) ? "YES" : "no") : "?");
            if (p) {
                const float *v = (const float *)il.object_unbox(p);
                fprintf(stderr, " pos=(%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
            }
            fprintf(stderr, "\n      chain:");
            // Up the parents, naming each and flagging the ones switched off.
            Il2CppObject *tf = invoke_on(vc, m_tf, NULL, "Component.transform");
            for (unsigned d = 0; tf && d < 12; d++) {
                Il2CppObject *go = invoke_on(tf, m_go, NULL, "Component.gameObject");
                if (!go) break;
                Il2CppObject *nm2 = invoke_on(go, m_objname, NULL, "Object.get_name");
                Il2CppObject *as = invoke_on(go, m_aself, NULL, "activeSelf");
                Il2CppObject *ah = invoke_on(go, m_ahier, NULL, "activeInHierarchy");
                char nb[96];
                str_ascii(nm2, nb, sizeof nb);
                int self = as ? *(uint8_t *)il.object_unbox(as) : -1;
                int hier = ah ? *(uint8_t *)il.object_unbox(ah) : -1;
                fprintf(stderr, " %s%s[self=%d,hier=%d]", d ? "< " : "", nb, self, hier);
                tf = invoke_on(tf, m_parent, NULL, "Transform.parent");
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    }

    // The other end of the chain: does the pointer ray actually land on UI?
    // Everything above measures what we hand the game; this reads back what
    // the game's own event system concluded, which is the difference between
    // "the controllers are wrong" and "the controllers are fine and the ray is
    // aimed somewhere else".
    static const MethodInfo *m_es_cur, *m_over, *m_sel, *m_name;
    static int resolved_es;
    if (!resolved_es) {
        resolved_es = 1;
        m_es_cur = find_method("UnityEngine.EventSystems", "EventSystem", "get_current", 0);
        m_over   = find_method("UnityEngine.EventSystems", "EventSystem",
                               "IsPointerOverGameObject", 0);
        m_sel    = find_method("UnityEngine.EventSystems", "EventSystem",
                               "get_currentSelectedGameObject", 0);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
    }

    // 1. Does any of the fill survive into Input.GetAxis? This was the one
    //    unproven link in the chain from our ovrp state to the game's click.
    fprintf(stderr, "  [mprobe f%u] axes:", frame);
    for (unsigned i = 0; i < N_AXES; i++) {
        void *args[1] = { axis_str[i] };
        fprintf(stderr, " %s=%.3f", AXES[i], invoke_float(m_axis, args, "Input.GetAxis"));
    }
    fprintf(stderr, "\n");

    // 2. And do the hand poses? The pointer ray is built from these, so a ray
    //    that never hits is either aimed wrong or cast from nowhere. Asked
    //    exactly the way VRController::Update asks it.
    Il2CppObject *c = invoke(m_conn, NULL, "OVRInput.GetConnectedControllers");
    fprintf(stderr, "  [mprobe f%u] OVRInput: connected=", frame);
    if (c) fprintf(stderr, "0x%x", *(uint32_t *)il.object_unbox(c));
    else   fprintf(stderr, "?");

    // OVRInput.Controller: LTouch = 0x01, RTouch = 0x02.
    static const struct { const char *name; int ctrl; } HANDS[] = {
        { "L", 0x01 }, { "R", 0x02 },
    };
    for (unsigned i = 0; i < 2; i++) {
        int ctrl = HANDS[i].ctrl;
        void *args[1] = { &ctrl };
        Il2CppObject *p  = invoke(m_cpos, args, "OVRInput.GetLocalControllerPosition");
        Il2CppObject *q  = invoke(m_crot, args, "OVRInput.GetLocalControllerRotation");
        Il2CppObject *pv = invoke(m_pvalid, args, "OVRInput.GetControllerPositionValid");
        Il2CppObject *pt = invoke(m_ptracked, args, "OVRInput.GetControllerPositionTracked");
        fprintf(stderr, " %s:", HANDS[i].name);
        if (pv) fprintf(stderr, "valid=%d", *(uint8_t *)il.object_unbox(pv));
        if (pt) fprintf(stderr, ",tracked=%d", *(uint8_t *)il.object_unbox(pt));
        if (p) {
            const float *v = (const float *)il.object_unbox(p);
            fprintf(stderr, ",pos=(%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
        }
        if (q) {
            const float *r = (const float *)il.object_unbox(q);
            fprintf(stderr, ",rot=(%.2f,%.2f,%.2f,%.2f)", r[0], r[1], r[2], r[3]);
        }
    }
    fprintf(stderr, "\n");

    // 3. The verdict: is the pointer over UI, and what does the game consider
    //    selected? A hit here is the whole point of the controller work.
    Il2CppObject *es = invoke(m_es_cur, NULL, "EventSystem.get_current");
    fprintf(stderr, "  [mprobe f%u] eventsystem:", frame);
    if (!es) {
        fprintf(stderr, " none\n");
    } else {
        Il2CppObject *over = invoke_on(es, m_over, NULL, "IsPointerOverGameObject");
        fprintf(stderr, " overUI=%s",
                over ? (*(uint8_t *)il.object_unbox(over) ? "YES" : "no") : "?");
        Il2CppObject *sel = invoke_on(es, m_sel, NULL, "get_currentSelectedGameObject");
        if (!sel) {
            fprintf(stderr, " selected=<none>");
        } else {
            Il2CppObject *nm = invoke_on(sel, m_name, NULL, "Object.get_name");
            char buf[128];
            str_ascii(nm, buf, sizeof buf);
            fprintf(stderr, " selected=\"%s\"", buf);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}
