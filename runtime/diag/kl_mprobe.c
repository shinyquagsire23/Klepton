#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_mprobe.h"
#include "kl_ovrp.h"

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
    Il2CppObject     *(*array_new)(Il2CppClass *, uintptr_t);
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
    BIND(array_new,                 "il2cpp_array_new");
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

// Gun Club VR only, self-gated on the target: its preference store is not
// flushed on the emulated exit path, so call the game's own public,
// parameterless static TBMPrefs.Save before Unity is torn down
// (KL_UNITY_QUIT_TEARDOWN, kl_driver.c). Runs on the thread that just ran
// nativeRender. What it writes is what the player chose; nothing is set here.
void kl_mprobe_gunclub_save_preferences(void) {
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "x2285803881494677") || !mprobe_init()) return;
    const MethodInfo *m = find_method("", "TBMPrefs", "Save", 0);
    if (!m) return;
    Il2CppObject *exc = NULL;
    il.runtime_invoke(m, NULL, NULL, &exc);
    if (exc) {
        Il2CppClass *k = il.object_get_class(exc);
        fprintf(stderr, "[guest] Gun Club VR: TBMPrefs.Save threw %s\n",
                k ? il.class_get_name(k) : "?");
    } else {
        fprintf(stderr, "[guest] Gun Club VR: TBMPrefs.Save returned\n");
    }
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

// Shave & Stuff uses BNG.InputBridge for UI and gameplay input. Its controller
// poses come from OculusXRPlugin, but every one of its managed input paths
// overwrites the BNG fields with zero before UI/gameplay reads them. Keep only
// this title's bridge in sync with the same physical controller snapshot sent
// through ovrp_GetControllerState4. InputSource 5 is deliberately outside the
// enum's 0...4 switch so BNG does not erase these values on the next frame.
// This runs after a guest frame, so the game observes a one-frame-old sample.
//
// The offsets below were measured on 1.12.3.2 (version code 197). Before the
// first write the object is checked against them: its class must be
// InputBridge, large enough for the highest offset, and every field this table
// names that IL2CPP can resolve must sit where it was measured. One that has
// moved means another build, and the bridge then stays off for the run rather
// than writing into the wrong fields.
static const struct { const char *name; size_t off; } k_bng_layout[] = {
    { "InputSource", 24 },      { "LeftGrip", 52 },        { "LeftGripDown", 56 },
    { "RightGrip", 60 },        { "RightGripDown", 64 },   { "LeftTrigger", 68 },
    { "LeftTriggerUp", 73 },    { "LeftTriggerDown", 74 }, { "RightTrigger", 76 },
    { "RightTriggerUp", 80 },   { "RightTriggerDown", 81 },
    { "LeftThumbstickDown", 85 },  { "LeftThumbstickUp", 86 },
    { "RightThumbstickDown", 87 }, { "RightThumbstickUp", 88 },
    { "LeftThumbstick", 89 },   { "RightThumbstick", 90 },
    { "AButton", 91 },          { "BButton", 104 },        { "XButton", 107 },
    { "YButton", 110 },         { "StartButton", 113 },    { "BackButton", 115 },
    { "LeftThumbstickAxis", 120 }, { "RightThumbstickAxis", 128 },
    { "IsOculusDevice", 212 },  { "IsOculusQuest", 213 },
};
#define BNG_LAYOUT_END 214      // one past the highest byte written below

static int shave_bridge_layout_ok(Il2CppObject *bridge) {
    Il2CppClass *k = il.object_get_class(bridge);
    const char *name = k ? il.class_get_name(k) : NULL;
    if (!name || strcmp(name, "InputBridge")) {
        fprintf(stderr, "  [mprobe] BNG bridge: object is %s, not InputBridge — off\n",
                name ? name : "?");
        return 0;
    }
    kl_image *img = kl_find_image("libil2cpp.so");
    typedef void *(*field_fn)(Il2CppClass *, const char *);
    typedef size_t (*f_off_fn)(void *);
    typedef int32_t (*size_fn)(Il2CppClass *);
    field_fn field = img ? (field_fn)kl_sym(img, "il2cpp_class_get_field_from_name") : NULL;
    f_off_fn f_off = img ? (f_off_fn)kl_sym(img, "il2cpp_field_get_offset") : NULL;
    size_fn isize = img ? (size_fn)kl_sym(img, "il2cpp_class_instance_size") : NULL;
    if (isize && isize(k) < BNG_LAYOUT_END) {
        fprintf(stderr, "  [mprobe] BNG bridge: InputBridge is %d bytes, the measured "
                        "layout needs %d — off\n", isize(k), BNG_LAYOUT_END);
        return 0;
    }
    unsigned named = 0, n = sizeof k_bng_layout / sizeof *k_bng_layout;
    for (unsigned i = 0; field && f_off && i < n; i++) {
        void *f = field(k, k_bng_layout[i].name);
        if (!f) continue;
        size_t at = f_off(f);
        if (at != k_bng_layout[i].off) {
            fprintf(stderr, "  [mprobe] BNG bridge: InputBridge.%s is at %zu, measured "
                            "%zu — not the 1.12.3.2 layout, off\n",
                    k_bng_layout[i].name, at, k_bng_layout[i].off);
            return 0;
        }
        named++;
    }
    fprintf(stderr, "  [mprobe] BNG bridge: on, %u of %u measured fields confirmed by name\n",
            named, n);
    return 1;
}

static void shave_input_bridge(void) {
    extern const char *kl_driver_target_name(void);
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x7532973430077887")) return;
    if (!mprobe_init()) return;

    static const MethodInfo *get_instance;
    static void *ui_bridge_field;
    static void (*static_get)(void *, void *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        get_instance = find_method("BNG", "InputBridge", "get_Instance", 0);
        kl_image *img = kl_find_image("libil2cpp.so");
        typedef void *(*field_fn)(Il2CppClass *, const char *);
        field_fn field = img ? (field_fn)kl_sym(
            img, "il2cpp_class_get_field_from_name") : NULL;
        static_get = img ? (void (*)(void *, void *))kl_sym(
            img, "il2cpp_field_static_get_value") : NULL;
        Il2CppClass *ui = find_class("", "UiPointerHelper");
        if (field && ui) ui_bridge_field = field(ui, "inputBridge");
    }
    // UiPointerHelper.IsUiPointerTriggerPressed reads this static pointer, not
    // InputBridge._instance directly. Scene changes may recreate it, so fetch
    // it every frame and only fall back to the singleton while UI is starting.
    Il2CppObject *bridge = NULL;
    if (ui_bridge_field && static_get) static_get(ui_bridge_field, &bridge);
    if (!bridge) bridge = invoke(get_instance, NULL, "BNG.InputBridge.Instance");
    if (!bridge) return;
    static int layout = -1;     // checked once: the class is the same every scene
    if (layout < 0) layout = shave_bridge_layout_ok(bridge);
    if (!layout) return;

    uint32_t lb = 0, rb = 0;
    float left = 0, right = 0, lg = 0, rg = 0;
    float lsx = 0, lsy = 0, rsx = 0, rsy = 0;
    kl_ovrp_controller_input(0, &lb, NULL, &left, &lg, &lsx, &lsy);
    kl_ovrp_controller_input(1, &rb, NULL, &right, &rg, &rsx, &rsy);
    uint8_t *p = (uint8_t *)bridge;
    *(int32_t *)(p + 24) = 5;                    // bypass BNG's zeroing switch
    p[212] = 1;                                  // IsOculusDevice
    p[213] = 1;                                  // IsOculusQuest

    memcpy(p + 52, &lg, sizeof lg);               // LeftGrip
    memcpy(p + 60, &rg, sizeof rg);               // RightGrip
    memcpy(p + 68, &left, sizeof left);
    memcpy(p + 76, &right, sizeof right);
    memcpy(p + 120, &lsx, sizeof lsx);             // LeftThumbstickAxis
    memcpy(p + 124, &lsy, sizeof lsy);
    memcpy(p + 128, &rsx, sizeof rsx);             // RightThumbstickAxis
    memcpy(p + 132, &rsy, sizeof rsy);

    static float old_left, old_right, old_lg, old_rg;
    static uint32_t old_lb, old_rb;
    const float threshold = 0.1f;
    int was_left = old_left >= threshold, now_left = left >= threshold;
    int was_right = old_right >= threshold, now_right = right >= threshold;
    p[56] = (uint8_t)(old_lg < threshold && lg >= threshold);
    p[64] = (uint8_t)(old_rg < threshold && rg >= threshold);
    p[73] = (uint8_t)(was_left && !now_left);     // LeftTriggerUp
    p[74] = (uint8_t)(!was_left && now_left);     // LeftTriggerDown
    p[80] = (uint8_t)(was_right && !now_right);   // RightTriggerUp
    p[81] = (uint8_t)(!was_right && now_right);   // RightTriggerDown

#define BNG_BUTTON(off, down, up, word, old, mask) do {                    \
        int now = ((word) & (mask)) != 0;                                  \
        int was = ((old) & (mask)) != 0;                                   \
        p[(off)] = (uint8_t)now;                                           \
        p[(down)] = (uint8_t)(!was && now);                                \
        p[(up)] = (uint8_t)(was && !now);                                  \
    } while (0)
    BNG_BUTTON(91, 92, 93, rb, old_rb, KL_OVRP_RAW_A);
    BNG_BUTTON(104, 105, 106, rb, old_rb, KL_OVRP_RAW_B);
    BNG_BUTTON(107, 108, 109, lb, old_lb, KL_OVRP_RAW_X);
    BNG_BUTTON(110, 111, 112, lb, old_lb, KL_OVRP_RAW_Y);
#undef BNG_BUTTON

    int menu = ((lb | rb) & (KL_OVRP_RAW_START | KL_OVRP_RAW_BACK)) != 0;
    int old_menu = ((old_lb | old_rb) &
                    (KL_OVRP_RAW_START | KL_OVRP_RAW_BACK)) != 0;
    p[94] = (uint8_t)menu;                       // MenuButton
    p[113] = (uint8_t)menu;                      // StartButton
    p[114] = (uint8_t)(!old_menu && menu);       // StartButtonDown
    p[115] = (uint8_t)menu;                      // BackButton
    p[116] = (uint8_t)(!old_menu && menu);       // BackButtonDown

    int lstick = (lb & KL_OVRP_RAW_LTHUMBSTICK) != 0;
    int rstick = (rb & KL_OVRP_RAW_RTHUMBSTICK) != 0;
    int old_lstick = (old_lb & KL_OVRP_RAW_LTHUMBSTICK) != 0;
    int old_rstick = (old_rb & KL_OVRP_RAW_RTHUMBSTICK) != 0;
    p[85] = (uint8_t)(!old_lstick && lstick);
    p[86] = (uint8_t)(old_lstick && !lstick);
    p[87] = (uint8_t)(!old_rstick && rstick);
    p[88] = (uint8_t)(old_rstick && !rstick);
    p[89] = (uint8_t)lstick;
    p[90] = (uint8_t)rstick;

    old_left = left;
    old_right = right;
    old_lg = lg;
    old_rg = rg;
    old_lb = lb;
    old_rb = rb;
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

// ---- KL_PROBE_METHODS: which methods of a class SURVIVED stripping ---------
//
// IL2CPP's managed stripping removes methods nothing in the build calls, and
// the metadata's string table cannot say which: a name there may belong to a
// field, a property or another class. The runtime can — il2cpp_class_get_methods
// walks exactly what this build compiled. Asked for the custom-level classes,
// whose shells are in the Quest build while their disk-reading half is not.
//
//   KL_PROBE_METHODS="CustomLevelLoader,BeatmapLevelsModel"
//
// Same spelling and same tick as KL_PROBE_ENUM. `ptr` is the methodPointer (a
// MethodInfo's first field), NULL for an abstract or generic-definition method.
static void probe_dump_methods_of(const char *spec) {
    char ns[128] = "", name[128] = "";
    const char *dot = strrchr(spec, '.');
    if (dot) {
        size_t nl = (size_t)(dot - spec);
        if (nl >= sizeof ns) nl = sizeof ns - 1;
        memcpy(ns, spec, nl); ns[nl] = 0;
        snprintf(name, sizeof name, "%s", dot + 1);
    } else {
        snprintf(name, sizeof name, "%s", spec);
    }
    kl_image *img = kl_find_image("libil2cpp.so");
    Il2CppClass *k = find_class(ns, name);
    if (!img || !k) { fprintf(stderr, "  [mprobe] methods %s: no such class\n", spec); return; }
    typedef const MethodInfo *(*get_methods_fn)(Il2CppClass *, void **);
    typedef const char *(*m_name_fn)(const MethodInfo *);
    typedef uint32_t (*m_argc_fn)(const MethodInfo *);
    typedef const void *(*m_param_fn)(const MethodInfo *, uint32_t);
    typedef char *(*t_name_fn)(const void *);
    typedef void *(*get_fields_fn)(Il2CppClass *, void **);
    typedef const char *(*f_name_fn)(void *);
    typedef const void *(*f_type_fn)(void *);
    typedef size_t (*f_off_fn)(void *);
    get_methods_fn get_methods = (get_methods_fn)kl_sym(img, "il2cpp_class_get_methods");
    m_name_fn      m_name      = (m_name_fn)kl_sym(img, "il2cpp_method_get_name");
    m_argc_fn      m_argc      = (m_argc_fn)kl_sym(img, "il2cpp_method_get_param_count");
    m_param_fn     m_param     = (m_param_fn)kl_sym(img, "il2cpp_method_get_param");
    t_name_fn      t_name      = (t_name_fn)kl_sym(img, "il2cpp_type_get_name");
    get_fields_fn  get_fields  = (get_fields_fn)kl_sym(img, "il2cpp_class_get_fields");
    f_name_fn      f_name      = (f_name_fn)kl_sym(img, "il2cpp_field_get_name");
    f_type_fn      f_type      = (f_type_fn)kl_sym(img, "il2cpp_field_get_type");
    f_off_fn       f_off       = (f_off_fn)kl_sym(img, "il2cpp_field_get_offset");
    if (!get_methods || !m_name) return;
    fprintf(stderr, "  [mprobe] methods of %s:\n", spec);
    void *it = NULL; unsigned n = 0;
    for (const MethodInfo *m; (m = get_methods(k, &it)) != NULL; n++) {
        void *ptr; memcpy(&ptr, m, sizeof ptr);
        unsigned argc = m_argc ? (unsigned)m_argc(m) : 0u;
        fprintf(stderr, "  [mprobe]   %-58s /%u ptr=%p\n", m_name(m), argc, ptr);
        // The parameter TYPES, which is what a caller building the argument
        // array by hand needs and what the name alone cannot give.
        if (m_param && t_name)
            for (unsigned i = 0; i < argc; i++) {
                const void *t = m_param(m, i);
                fprintf(stderr, "  [mprobe]       arg%-2u %s\n", i, t ? t_name(t) : "?");
            }
    }
    fprintf(stderr, "  [mprobe]   (%u method(s))\n", n);
    // ...and the fields, with offsets: the half of a class that reflection by
    // method name cannot reach when the accessor was stripped.
    if (get_fields && f_name && f_type && f_off && t_name) {
        fprintf(stderr, "  [mprobe] fields of %s:\n", spec);
        void *fit = NULL;
        for (void *f; (f = get_fields(k, &fit)) != NULL; ) {
            const void *t = f_type(f);
            fprintf(stderr, "  [mprobe]   +0x%03zx %-44s %s\n", f_off(f), f_name(f),
                    t ? t_name(t) : "?");
        }
    }
}

static void probe_dump_methods(void) {
    const char *spec = kl_env_str("KL_PROBE_METHODS", NULL);
    if (!spec || !*spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf, *tok; (tok = strsep(&p, ",")) != NULL; )
        if (*tok) probe_dump_methods_of(tok);
}

// ---- KL_PROBE_FIELDS: the instance fields of a live component --------------
//
// KL_PROBE_XFORM says WHICH transform went NaN; the component that writes it
// keeps the inputs of that arithmetic in private fields no accessor exposes
// (a calibrated height, a weight, a cached pose). Take the component off the
// scene object that carries it and print its value-typed fields raw, by offset.
// Object.FindObjectOfType would spare the path, but stripping took it.
//
//   KL_PROBE_FIELDS="PlayerController(Clone)/TrackingOffset=Camouflaj.XRFramework.Tracking.OffsetTracker"
//
// Same cadence as KL_PROBE_XFORM, at most KL_PROBE_FIELDS_MAX (8) times.
static void probe_fields_of(unsigned frame, const char *item) {
    char path[256];
    snprintf(path, sizeof path, "%s", item);
    char *eq = strchr(path, '=');
    if (!eq) { fprintf(stderr, "  [mprobe] fields %s: want <scene path>=<Namespace.Class>\n", item); return; }
    *eq = 0;
    const char *spec = eq + 1;
    char ns[128] = "", name[128] = "";
    const char *dot = strrchr(spec, '.');
    if (dot) {
        size_t nl = (size_t)(dot - spec);
        if (nl >= sizeof ns) nl = sizeof ns - 1;
        memcpy(ns, spec, nl); ns[nl] = 0;
        snprintf(name, sizeof name, "%s", dot + 1);
    } else {
        snprintf(name, sizeof name, "%s", spec);
    }
    kl_image *img = kl_find_image("libil2cpp.so");
    Il2CppClass *k = find_class(ns, name);
    if (!img || !k) { fprintf(stderr, "  [mprobe] fields %s: no such class\n", spec); return; }
    typedef char *(*t_name_fn)(const void *);
    typedef void *(*get_fields_fn)(Il2CppClass *, void **);
    typedef const char *(*f_name_fn)(void *);
    typedef const void *(*f_type_fn)(void *);
    typedef size_t (*f_off_fn)(void *);
    typedef int (*f_flags_fn)(void *);
    t_name_fn      t_name     = (t_name_fn)kl_sym(img, "il2cpp_type_get_name");
    get_fields_fn  get_fields = (get_fields_fn)kl_sym(img, "il2cpp_class_get_fields");
    f_name_fn      f_name     = (f_name_fn)kl_sym(img, "il2cpp_field_get_name");
    f_type_fn      f_type     = (f_type_fn)kl_sym(img, "il2cpp_field_get_type");
    f_off_fn       f_off      = (f_off_fn)kl_sym(img, "il2cpp_field_get_offset");
    f_flags_fn     f_flags    = (f_flags_fn)kl_sym(img, "il2cpp_field_get_flags");
    if (!t_name || !get_fields || !f_name || !f_type || !f_off || !f_flags) return;
    // GetComponent has a (Type) and a (String) overload with the same arity;
    // the by-name lookup returns whichever comes first. Pick by parameter type.
    typedef const MethodInfo *(*get_methods_fn)(Il2CppClass *, void **);
    typedef const char *(*m_name_fn)(const MethodInfo *);
    typedef uint32_t (*m_argc_fn)(const MethodInfo *);
    typedef const void *(*m_param_fn)(const MethodInfo *, uint32_t);
    static const MethodInfo *m_find, *m_getc;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_find = find_method("UnityEngine", "GameObject", "Find", 1);
        get_methods_fn get_methods = (get_methods_fn)kl_sym(img, "il2cpp_class_get_methods");
        m_name_fn      m_name      = (m_name_fn)kl_sym(img, "il2cpp_method_get_name");
        m_argc_fn      m_argc      = (m_argc_fn)kl_sym(img, "il2cpp_method_get_param_count");
        m_param_fn     m_param     = (m_param_fn)kl_sym(img, "il2cpp_method_get_param");
        Il2CppClass *gok = find_class("UnityEngine", "GameObject");
        void *it = NULL;
        if (gok && get_methods && m_name && m_argc && m_param)
            for (const MethodInfo *m; (m = get_methods(gok, &it)) != NULL; ) {
                if (strcmp(m_name(m), "GetComponent") || m_argc(m) != 1) continue;
                const void *t = m_param(m, 0);
                if (t && !strcmp(t_name(t), "System.Type")) { m_getc = m; break; }
            }
        if (!m_getc) fprintf(stderr, "  [mprobe] GameObject has no GetComponent(Type)\n");
    }
    if (!m_find || !m_getc) return;
    void *a0[1] = { il.string_new(path) };
    Il2CppObject *go = invoke(m_find, a0, "GameObject.Find");
    if (!go) return;                                  // not spawned yet: stay quiet
    void *a1[1] = { il.type_get_object(il.class_get_type(k)) };
    Il2CppObject *obj = invoke_on(go, m_getc, a1, "GameObject.GetComponent");
    if (!obj) { fprintf(stderr, "  [mprobe f%u] fields %s: %s carries none\n", frame, spec, path); return; }
    fprintf(stderr, "  [mprobe f%u] fields of the live %s @%p:\n", frame, spec, (void *)obj);
    const uint8_t *base = (const uint8_t *)obj;
    void *fit = NULL;
    for (void *f; (f = get_fields(k, &fit)) != NULL; ) {
        if (f_flags(f) & 0x10) continue;                  // static: not in the instance
        const void *t = f_type(f);
        const char *tn = t ? t_name(t) : "?";
        size_t off = f_off(f);
        const uint8_t *p = base + off;
        float v[4]; int32_t i32; memcpy(v, p, sizeof v); memcpy(&i32, p, sizeof i32);
        if (!strcmp(tn, "System.Single"))
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s %g\n", off, f_name(f), v[0]);
        else if (!strcmp(tn, "System.Boolean"))
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s %s\n", off, f_name(f), p[0] ? "true" : "false");
        else if (!strcmp(tn, "UnityEngine.Vector3"))
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s (%g %g %g)\n", off, f_name(f), v[0], v[1], v[2]);
        else if (!strcmp(tn, "UnityEngine.Quaternion"))
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s (%g %g %g %g)\n", off, f_name(f), v[0], v[1], v[2], v[3]);
        else if (!strcmp(tn, "System.Int32") || strstr(tn, "Flags") || strstr(tn, "Type") || strstr(tn, "Mode"))
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s %d   (%s)\n", off, f_name(f), i32, tn);
        else if (!strncmp(tn, "System.Nullable<", 16))
            // Mono lays Nullable<T> out as { T value; bool has_value }.
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s raw(%g %g %g %g)   (%s)\n", off, f_name(f),
                    v[0], v[1], v[2], v[3], tn);
        else {
            void *ref; memcpy(&ref, p, sizeof ref);
            fprintf(stderr, "  [mprobe]   +0x%03zx %-34s %s   (%s)\n", off, f_name(f),
                    ref ? "set" : "null", tn);
        }
    }
}

static void probe_fields(unsigned frame) {
    const char *spec = kl_env_str("KL_PROBE_FIELDS", NULL);
    static unsigned printed, cap;
    if (!cap) cap = kl_env_uint("KL_PROBE_FIELDS_MAX", 8u);
    if (!spec || !*spec || printed >= cap) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf, *tok; (tok = strsep(&p, ",")) != NULL; )
        if (*tok) probe_fields_of(frame, tok);
    printed++;
}

// ---- KL_PROBE_XFORM: the local TRS along a scene path ----------------------
//
// Unity says "Infinity or NaN ... transform matrix" and names the LEAF of a
// hierarchy path. Which ancestor went bad first, and whether it is its
// position, rotation or scale, is what decides where to look — a NaN rotation
// is a pose source, a zero or NaN scale is something else entirely — and the
// message carries none of it. So walk the path from the root object down and
// print each level's own local values.
//
//   KL_PROBE_XFORM="PlayerController(Clone)/TrackingOffset/TrackedHeadset/Camera"
//
// Printed on the probe's cadence (KL_PROBE_FROM / KL_PROBE_EVERY), at most
// eight times, and only once the root object exists.
static void probe_xform_path(unsigned frame) {
    const char *spec = kl_env_str("KL_PROBE_XFORM", NULL);
    static unsigned printed;
    if (!spec || !*spec || printed >= 8) return;
    static const MethodInfo *m_find, *m_tr, *m_child, *m_lp, *m_lr, *m_ls, *m_wp;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_find  = find_method("UnityEngine", "GameObject", "Find", 1);
        m_tr    = find_method("UnityEngine", "GameObject", "get_transform", 0);
        m_child = find_method("UnityEngine", "Transform", "Find", 1);
        m_lp    = find_method("UnityEngine", "Transform", "get_localPosition", 0);
        m_lr    = find_method("UnityEngine", "Transform", "get_localRotation", 0);
        m_ls    = find_method("UnityEngine", "Transform", "get_localScale", 0);
        m_wp    = find_method("UnityEngine", "Transform", "get_position", 0);
    }
    if (!m_find || !m_tr || !m_child) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    char *rest = buf, *seg = strsep(&rest, "/");
    void *a1[1] = { il.string_new(seg) };
    Il2CppObject *go = invoke(m_find, a1, "GameObject.Find");
    if (!go) return;                                  // not spawned yet: stay quiet
    Il2CppObject *t = invoke_on(go, m_tr, NULL, "GameObject.transform");
    fprintf(stderr, "  [mprobe f%u] xform path %s\n", frame, spec);
    for (int depth = 0; t; depth++) {
        float lp[3] = {0}, lr[4] = {0}, ls[3] = {0}, wp[3] = {0};
        Il2CppObject *o;
        if (m_lp && (o = invoke_on(t, m_lp, NULL, "localPosition"))) memcpy(lp, il.object_unbox(o), sizeof lp);
        if (m_lr && (o = invoke_on(t, m_lr, NULL, "localRotation"))) memcpy(lr, il.object_unbox(o), sizeof lr);
        if (m_ls && (o = invoke_on(t, m_ls, NULL, "localScale")))    memcpy(ls, il.object_unbox(o), sizeof ls);
        if (m_wp && (o = invoke_on(t, m_wp, NULL, "position")))      memcpy(wp, il.object_unbox(o), sizeof wp);
        fprintf(stderr, "  [mprobe]   %*s%-18s lpos(%g %g %g) lrot(%g %g %g %g) lscale(%g %g %g) wpos(%g %g %g)\n",
                depth * 2, "", seg, lp[0], lp[1], lp[2], lr[0], lr[1], lr[2], lr[3],
                ls[0], ls[1], ls[2], wp[0], wp[1], wp[2]);
        seg = rest ? strsep(&rest, "/") : NULL;
        if (!seg) break;
        void *a2[1] = { il.string_new(seg) };
        t = invoke_on(t, m_child, a2, "Transform.Find");
        if (!t) fprintf(stderr, "  [mprobe]   %*s%s: no such child\n", depth * 2 + 2, "", seg);
    }
    printed++;
}

static void probe_dump_enums(void) {
    const char *spec = kl_env_str("KL_PROBE_ENUM", NULL);
    if (!spec || !*spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf, *tok; (tok = strsep(&p, ",")) != NULL; )
        if (*tok) probe_dump_enum(tok);
}

// The axes this title binds (parsed out of globalgamemanagers).
static const char *const AXES[] = {
    "TriggerLeftHand", "TriggerRightHand",
    "HorizontalLeftHand", "VerticalLeftHand",
    "HorizontalRightHand", "VerticalRightHand",
    "MenuButtonLeftHand", "MenuButtonRightHand",
};
#define N_AXES (sizeof AXES / sizeof AXES[0])

// ---- KL_PROBE_STACKTRACE: make Unity's own exception log say WHERE ---------
//
// A managed exception that prints its message and nothing else is a name
// without an address. BONELAB throws `ArgumentOutOfRangeException` once a
// frame out of Unity's SRP `XRSystem` and the log carries three blank lines
// where the stack should be — which has two very different causes, and they
// are not distinguishable from the native side: either the build strips
// managed stack traces, or the game called `Application.SetStackTraceLogType`
// to turn them off. `Application.GetStackTraceLogType` answers that, so this
// PRINTS the setting it found before changing it: a probe that reports
// "already ScriptOnly" is telling you the traces are genuinely unavailable and
// that this instrument is a dead end, which is worth as much as a trace.
//
// StackTraceLogType { None = 0, ScriptOnly = 1, Full = 2 } and
// LogType { Error = 0, Assert = 1, Warning = 2, Log = 3, Exception = 4 } — both
// are UnityEngine enums stable since 5.x. Enums marshal as their underlying
// int, so il2cpp_runtime_invoke takes the ADDRESS of each value.
//
// Diagnostic only, and it changes the guest's logging configuration, so it is
// off unless asked for.
static void probe_stack_traces(void) {
    static const char *const LOGTYPE[] = { "Error", "Assert", "Warning", "Log",
                                           "Exception" };
    static const char *const STLT[] = { "None", "ScriptOnly", "Full" };

    const MethodInfo *m_get = find_method("UnityEngine", "Application",
                                          "GetStackTraceLogType", 1);
    const MethodInfo *m_set = find_method("UnityEngine", "Application",
                                          "SetStackTraceLogType", 2);
    if (!m_get || !m_set) return;

    // What the guest asked for, before we touch it.
    int want = (int)kl_env_uint("KL_PROBE_STACKTRACE", 1);
    if (want < 0 || want > 2) want = 1;
    for (int lt = 0; lt < 5; lt++) {
        int arg = lt;
        void *ga[1] = { &arg };
        Il2CppObject *r = invoke(m_get, ga, "Application.GetStackTraceLogType");
        int had = r ? *(int32_t *)il.object_unbox(r) : -1;
        int set = want;
        void *sa[2] = { &arg, &set };
        invoke(m_set, sa, "Application.SetStackTraceLogType");
        fprintf(stderr, "  [mprobe] stack traces for %-9s: %s -> %s\n",
                LOGTYPE[lt],
                (had >= 0 && had <= 2) ? STLT[had] : "?",
                STLT[want]);
    }
}

// ---- KL_PROBE_XR: what Unity thinks the XR display IS ---------------------
//
// Every number on this path is one WE answered — the eye texture size through
// ovrp_CalculateEyeLayerDesc, the layout through ovrp_GetSystemMultiViewSupported,
// the viewport through ovrp_CalculateEyeViewportRect — and they travel through
// libunity and the Oculus XR provider before Unity's own render loop reads them
// back. That round trip is where they can stop agreeing, and nothing on the
// native side can see the far end of it: our log says what we said, not what
// Unity concluded. `XRSettings` is Unity's conclusion, in Unity's words.
//
// The reason this exists: BONELAB throws `ArgumentOutOfRangeException` once per
// frame out of the SRP render loop with an EMPTY stack trace (the build carries
// none — `KL_PROBE_STACKTRACE` measured that and is a dead end), so the throw
// site cannot be named from either side. What can be named is a disagreement,
// and the first candidate is the stereo mode: a title authored for Single Pass
// Instanced whose display hands back two separate 2D textures is a shape its
// renderer may never have run.
static void probe_xr_display(unsigned frame) {
    // UnityEngine.XR.StereoRenderingMode { MultiPass = 0, SinglePass = 1,
    // SinglePassInstanced = 2, SinglePassMultiview = 3 }.
    static const char *const SRM[] = { "MultiPass", "SinglePass",
                                       "SinglePassInstanced",
                                       "SinglePassMultiview" };
    static const MethodInfo *m_mode, *m_w, *m_h, *m_vps, *m_en, *m_dev, *m_occ;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_mode = find_method("UnityEngine.XR", "XRSettings",
                             "get_stereoRenderingMode", 0);
        m_w    = find_method("UnityEngine.XR", "XRSettings", "get_eyeTextureWidth", 0);
        m_h    = find_method("UnityEngine.XR", "XRSettings", "get_eyeTextureHeight", 0);
        m_vps  = find_method("UnityEngine.XR", "XRSettings",
                             "get_renderViewportScale", 0);
        m_en   = find_method("UnityEngine.XR", "XRSettings", "get_enabled", 0);
        m_dev  = find_method("UnityEngine.XR", "XRSettings", "get_loadedDeviceName", 0);
        m_occ  = find_method("UnityEngine.XR", "XRSettings",
                             "get_useOcclusionMesh", 0);
    }
    Il2CppObject *mo = invoke(m_mode, NULL, "XRSettings.stereoRenderingMode");
    Il2CppObject *wo = invoke(m_w,    NULL, "XRSettings.eyeTextureWidth");
    Il2CppObject *ho = invoke(m_h,    NULL, "XRSettings.eyeTextureHeight");
    Il2CppObject *so = invoke(m_vps,  NULL, "XRSettings.renderViewportScale");
    Il2CppObject *eo = invoke(m_en,   NULL, "XRSettings.enabled");
    Il2CppObject *oo = invoke(m_occ,  NULL, "XRSettings.useOcclusionMesh");
    Il2CppObject *dv = invoke(m_dev,  NULL, "XRSettings.loadedDeviceName");
    int mode = mo ? *(int32_t *)il.object_unbox(mo) : -1;
    char dn[128] = "?";
    if (dv) str_ascii(dv, dn, sizeof dn);
    fprintf(stderr, "  [mprobe f%u] XRSettings: device=\"%s\" enabled=%s "
                    "stereo=%d (%s) eyeTexture=%dx%d viewportScale=%.3f "
                    "occlusionMesh=%s\n",
            frame, dn,
            eo ? (*(uint8_t *)il.object_unbox(eo) ? "1" : "0") : "?",
            mode, (mode >= 0 && mode <= 3) ? SRM[mode] : "?",
            wo ? *(int32_t *)il.object_unbox(wo) : -1,
            ho ? *(int32_t *)il.object_unbox(ho) : -1,
            so ? (double)*(float *)il.object_unbox(so) : -1.0,
            oo ? (*(uint8_t *)il.object_unbox(oo) ? "1" : "0") : "?");
}

// ---------------------------------------------------------------------------
// KL_PROBE_RESOLVE=0xoff[,0xoff...]: name the managed method CONTAINING each
// given libil2cpp offset. The sampler reports parked pcs as libil2cpp+0x...;
// on metadata v29 its own resolver cannot name them, but the RUNTIME can:
// enumerate every image -> class -> method through the embedding API, read
// each MethodInfo's methodPointer (its first field - stable across il2cpp
// versions), and keep the nearest method at or below each target. One pass,
// a few seconds, printed once.
// KL_PROBE_CLASSAT=<hex>[,<hex>...] - which class a metadata slot of libil2cpp
// holds. Compiled code reaches a class through such a slot (`adrp`/`ldr` of a
// fixed address, then `[x, #0xb8]` for its statics); once the function has run,
// the slot holds the Il2CppClass. Printed with the class's static fields, which
// is what a disassembled `ldrb w8, [statics]` is reading.
static void mprobe_class_at(void) {
    const char *spec = kl_env_str("KL_PROBE_CLASSAT", NULL);
    if (!spec || !*spec) return;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return;
    typedef const char *(*k_ns_fn)(Il2CppClass *);
    typedef void *(*get_fields_fn)(Il2CppClass *, void **);
    typedef const char *(*f_name_fn)(void *);
    typedef size_t (*f_off_fn)(void *);
    typedef int (*f_flags_fn)(void *);
    k_ns_fn k_ns = (k_ns_fn)kl_sym(img, "il2cpp_class_get_namespace");
    get_fields_fn get_fields = (get_fields_fn)kl_sym(img, "il2cpp_class_get_fields");
    f_name_fn f_name = (f_name_fn)kl_sym(img, "il2cpp_field_get_name");
    f_off_fn f_off = (f_off_fn)kl_sym(img, "il2cpp_field_get_offset");
    f_flags_fn f_flags = (f_flags_fn)kl_sym(img, "il2cpp_field_get_flags");
    uintptr_t base = (uintptr_t)kl_base(img);
    for (const char *p = spec; *p; ) {
        uintptr_t off = (uintptr_t)strtoull(p, NULL, 16);
        // The slot holds the address of the class's own variable (the code does
        // `ldr x8, [slot]; ldr x8, [x8]`), and that variable holds the class.
        uintptr_t var = *(uintptr_t *)(base + off);
        uintptr_t v = (var >= 0x100000000ull && !(var & 7)) ? *(uintptr_t *)var : 0;
        // A variable not yet initialised holds an encoded metadata index, not a pointer.
        if (v < 0x100000000ull || (v & 7)) {
            fprintf(stderr, "  [mprobe] class at %#llx: not initialised yet (%#llx)\n", (unsigned long long)off, (unsigned long long)v);
        } else {
            Il2CppClass *k = (Il2CppClass *)v;
            const char *ns = k_ns ? k_ns(k) : "";
            fprintf(stderr, "  [mprobe] class at %#llx: %s%s%s\n", (unsigned long long)off, ns ? ns : "", ns && *ns ? "." : "", il.class_get_name(k));
            // `<slot>:<offset>`: the static at that offset is an object - say what it is.
            const char *colon = p;
            while (*colon && *colon != ',' && *colon != ':') colon++;
            if (*colon == ':') {
                uintptr_t statics = *(uintptr_t *)((uintptr_t)k + 0xb8);
                uintptr_t obj = statics >= 0x100000000ull ? *(uintptr_t *)(statics + strtoul(colon + 1, NULL, 0)) : 0;
                if (obj >= 0x100000000ull && !(obj & 7)) {
                    Il2CppClass *ok = *(Il2CppClass **)obj;
                    const char *ons = k_ns ? k_ns(ok) : "";
                    fprintf(stderr, "  [mprobe]     object at static %lu: %s%s%s (%p)\n", strtoul(colon + 1, NULL, 0),
                            ons ? ons : "", ons && *ons ? "." : "", il.class_get_name(ok), (void *)obj);
                } else {
                    fprintf(stderr, "  [mprobe]     object at static %lu: none\n", strtoul(colon + 1, NULL, 0));
                }
            }
            void *it = NULL, *fld;
            while (get_fields && f_name && (fld = get_fields(k, &it)) != NULL)
                if (f_flags && (f_flags(fld) & 0x10))
                    fprintf(stderr, "  [mprobe]     static %s (offset %zu)\n", f_name(fld), f_off ? f_off(fld) : 0);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

static void mprobe_resolve_offsets(void) {
    const char *spec = kl_env_str("KL_PROBE_RESOLVE", NULL);
    if (!spec || !*spec) return;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return;
    typedef const MethodInfo *(*get_methods_fn)(Il2CppClass *, void **);
    typedef size_t (*class_count_fn)(const Il2CppImage *);
    typedef Il2CppClass *(*get_class_fn)(const Il2CppImage *, size_t);
    typedef const char *(*m_name_fn)(const MethodInfo *);
    typedef const char *(*i_name_fn)(const Il2CppImage *);
    get_methods_fn get_methods = (get_methods_fn)kl_sym(img, "il2cpp_class_get_methods");
    class_count_fn class_count = (class_count_fn)kl_sym(img, "il2cpp_image_get_class_count");
    get_class_fn   get_class   = (get_class_fn)kl_sym(img, "il2cpp_image_get_class");
    m_name_fn      m_name      = (m_name_fn)kl_sym(img, "il2cpp_method_get_name");
    i_name_fn      i_name      = (i_name_fn)kl_sym(img, "il2cpp_image_get_name");
    if (!get_methods || !class_count || !get_class || !m_name) {
        fprintf(stderr, "  [mprobe] resolve: enumeration exports missing\n");
        return;
    }
    uintptr_t base = (uintptr_t)kl_base(img);
    enum { MAXT = 96 };
    uintptr_t tgt[MAXT]; int nt = 0;
    for (const char *p = spec; *p && nt < MAXT; ) {
        tgt[nt++] = base + (uintptr_t)strtoull(p, NULL, 16);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    struct { uintptr_t best; const MethodInfo *m; Il2CppClass *k; const Il2CppImage *im; } hit[MAXT];
    memset(hit, 0, sizeof hit);
    size_t na = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(il.domain_get(), &na);
    unsigned long long nmeth = 0;
    for (size_t a = 0; a < na; a++) {
        const Il2CppImage *im = il.assembly_get_image(as[a]);
        if (!im) continue;
        size_t nc = class_count(im);
        for (size_t c = 0; c < nc; c++) {
            Il2CppClass *k = get_class(im, c);
            if (!k) continue;
            void *iter = NULL;
            const MethodInfo *m;
            while ((m = get_methods(k, &iter)) != NULL) {
                nmeth++;
                uintptr_t p = *(uintptr_t *)m;   // MethodInfo.methodPointer
                if (!p) continue;
                for (int t = 0; t < nt; t++)
                    if (p <= tgt[t] && p > hit[t].best) {
                        hit[t].best = p; hit[t].m = m; hit[t].k = k; hit[t].im = im;
                    }
            }
        }
    }
    fprintf(stderr, "  [mprobe] resolve: scanned %llu methods in %zu assemblies\n",
            nmeth, na);
    for (int t = 0; t < nt; t++) {
        if (!hit[t].m) { fprintf(stderr, "  [mprobe] resolve %#llx: no method at or below\n",
                                 (unsigned long long)(tgt[t] - base)); continue; }
        fprintf(stderr, "  [mprobe] resolve libil2cpp+%#llx -> %s::%s (+%#llx, image %s)\n",
                (unsigned long long)(tgt[t] - base),
                il.class_get_name(hit[t].k), m_name(hit[t].m),
                (unsigned long long)(tgt[t] - hit[t].best),
                i_name && hit[t].im ? i_name(hit[t].im) : "?");
    }
}

// ---------------------------------------------------------------------------
// KL_PROBE_FIND=<part>[,<part>...]: every managed method whose NAME contains one
// of the parts, with the class that declares it — the first step toward calling
// something in a guest whose code is only names in global-metadata.dat. One
// pass over every image, printed once: `Namespace.Class::Method(types) static`.
static void mprobe_find_methods(void) {
    const char *spec = kl_env_str("KL_PROBE_FIND", NULL);
    if (!spec || !*spec) return;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return;
    typedef const MethodInfo *(*get_methods_fn)(Il2CppClass *, void **);
    typedef size_t (*class_count_fn)(const Il2CppImage *);
    typedef Il2CppClass *(*get_class_fn)(const Il2CppImage *, size_t);
    typedef const char *(*m_name_fn)(const MethodInfo *);
    typedef const char *(*k_ns_fn)(Il2CppClass *);
    typedef uint32_t (*m_argc_fn)(const MethodInfo *);
    typedef void *(*m_param_fn)(const MethodInfo *, uint32_t);
    typedef char *(*t_name_fn)(void *);
    typedef uint32_t (*m_flags_fn)(const MethodInfo *, uint32_t *);
    get_methods_fn get_methods = (get_methods_fn)kl_sym(img, "il2cpp_class_get_methods");
    class_count_fn class_count = (class_count_fn)kl_sym(img, "il2cpp_image_get_class_count");
    get_class_fn   get_class   = (get_class_fn)kl_sym(img, "il2cpp_image_get_class");
    m_name_fn      m_name      = (m_name_fn)kl_sym(img, "il2cpp_method_get_name");
    k_ns_fn        k_ns        = (k_ns_fn)kl_sym(img, "il2cpp_class_get_namespace");
    m_argc_fn      m_argc      = (m_argc_fn)kl_sym(img, "il2cpp_method_get_param_count");
    m_param_fn     m_param     = (m_param_fn)kl_sym(img, "il2cpp_method_get_param");
    t_name_fn      t_name      = (t_name_fn)kl_sym(img, "il2cpp_type_get_name");
    m_flags_fn     m_flags     = (m_flags_fn)kl_sym(img, "il2cpp_method_get_flags");
    typedef void *(*get_fields_fn)(Il2CppClass *, void **);
    typedef const char *(*f_name_fn)(void *);
    typedef void *(*f_type_fn)(void *);
    typedef size_t (*f_off_fn)(void *);
    typedef int (*f_flags_fn)(void *);
    get_fields_fn get_fields = (get_fields_fn)kl_sym(img, "il2cpp_class_get_fields");
    f_name_fn     f_name     = (f_name_fn)kl_sym(img, "il2cpp_field_get_name");
    f_type_fn     f_type     = (f_type_fn)kl_sym(img, "il2cpp_field_get_type");
    f_off_fn      f_off      = (f_off_fn)kl_sym(img, "il2cpp_field_get_offset");
    f_flags_fn    f_flags    = (f_flags_fn)kl_sym(img, "il2cpp_field_get_flags");
    if (!get_methods || !class_count || !get_class || !m_name) {
        fprintf(stderr, "  [mprobe] find: enumeration exports missing\n");
        return;
    }
    char parts[512];
    snprintf(parts, sizeof parts, "%s", spec);
    const char *want[16]; int nw = 0;
    for (char *t = strtok(parts, ","); t && nw < 16; t = strtok(NULL, ",")) want[nw++] = t;
    size_t na = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(il.domain_get(), &na);
    unsigned found = 0;
    for (size_t a = 0; a < na; a++) {
        const Il2CppImage *im = il.assembly_get_image(as[a]);
        if (!im) continue;
        size_t nc = class_count(im);
        for (size_t c = 0; c < nc; c++) {
            Il2CppClass *k = get_class(im, c);
            if (!k) continue;
            // ...and for a class asked for with '@', its fields too: what a method
            // works on is as much of the picture as what it is called.
            if (get_fields && f_name) {
                char cfull[300];
                { const char *n0 = k_ns ? k_ns(k) : ""; snprintf(cfull, sizeof cfull, "%s.%s", n0 ? n0 : "", il.class_get_name(k)); }
                int chit = 0;
                for (int w = 0; w < nw && !chit; w++) chit = want[w][0] == '@' && strstr(cfull, want[w] + 1) != NULL;
                void *fit = NULL, *fld;
                while (chit && (fld = get_fields(k, &fit)) != NULL) {
                    char *tn = (f_type && t_name) ? t_name(f_type(fld)) : NULL;
                    fprintf(stderr, "  [mprobe] find: %s  field %s : %s  (offset %zu%s)\n", cfull, f_name(fld), tn ? tn : "?",
                            f_off ? f_off(fld) : 0, (f_flags && (f_flags(fld) & 0x10)) ? ", static" : "");
                }
            }
            void *iter = NULL;
            const MethodInfo *m;
            while ((m = get_methods(k, &iter)) != NULL) {
                const char *name = m_name(m);
                int hit = 0;
                // A part that starts with '@' is matched against the class
                // ("@Mighty.Tester." lists everything a namespace declares).
                char full[300];
                { const char *n0 = k_ns ? k_ns(k) : ""; snprintf(full, sizeof full, "%s.%s", n0 ? n0 : "", il.class_get_name(k)); }
                for (int w = 0; name && w < nw && !hit; w++)
                    hit = want[w][0] == '@' ? strstr(full, want[w] + 1) != NULL : strstr(name, want[w]) != NULL;
                if (!hit) continue;
                char sig[400] = ""; size_t o = 0;
                uint32_t argc = m_argc ? m_argc(m) : 0;
                for (uint32_t i = 0; i < argc && m_param && t_name && o + 2 < sizeof sig; i++) {
                    char *tn = t_name(m_param(m, i));
                    o += (size_t)snprintf(sig + o, sizeof sig - o, "%s%s", i ? ", " : "", tn ? tn : "?");
                    if (o >= sizeof sig) o = sizeof sig - 1;
                }
                uint32_t fl = m_flags ? m_flags(m, NULL) : 0;
                const char *ns = k_ns ? k_ns(k) : "";
                // Where its code is, as an offset into the file: where to start
                // `llvm-objdump -d --start-address=` on it. (MethodInfo.methodPointer
                // is its first field, see mprobe_resolve_offsets.)
                char at[48] = "";
                {
                    kl_image *lib = kl_find_image("libil2cpp.so");
                    uintptr_t code = *(uintptr_t *)m;
                    if (lib && code) snprintf(at, sizeof at, "  @libil2cpp+%#llx", (unsigned long long)(code - (uintptr_t)kl_base(lib)));
                }
                fprintf(stderr, "  [mprobe] find: %s%s%s::%s(%s)%s%s\n", ns ? ns : "", ns && *ns ? "." : "",
                        il.class_get_name(k), name, sig, (fl & 0x10) ? " static" : "", at);
                found++;
            }
        }
    }
    fprintf(stderr, "  [mprobe] find: %u method(s) in %zu assemblies\n", found, na);
}

// ---------------------------------------------------------------------------
// Every object of a type in the loaded scenes, inactive ones too. The three-
// argument FindObjectsByType is asked for by its argument count because it is
// the one overload of the family that count does not share with a generic one:
// FindObjectOfType(Type) does, and a lookup by name and count may hand back
// FindObjectOfType<T>(bool) instead — which is how this probe once said "no
// such object" of objects that were there.
static Il2CppObject *mprobe_objects(Il2CppObject *type, uint32_t *count) {
    *count = 0;
    const MethodInfo *fa = find_method("UnityEngine", "Object", "FindObjectsByType", 3);
    if (!fa && type) {
        // Unity before 2020.3 has no FindObjectsByType (Elixir, 2019.2):
        // Resources.FindObjectsOfTypeAll, which includes inactive objects too
        // (and prefabs, which a probe of a running scene rarely meets).
        const MethodInfo *fall = find_method("UnityEngine", "Resources", "FindObjectsOfTypeAll", 1);
        if (!fall) return NULL;
        void *a1[1] = { type };
        Il2CppObject *arr = invoke(fall, a1, "Resources.FindObjectsOfTypeAll");
        if (arr) *count = *(uint32_t *)((char *)arr + 24);
        return arr;
    }
    if (!fa || !type) return NULL;
    int32_t inactive = 1 /* FindObjectsInactive.Include */, sort = 1 /* FindObjectsSortMode.InstanceID: the same order each time */;
    void *args[3] = { type, &inactive, &sort };
    Il2CppObject *arr = invoke(fa, args, "Object.FindObjectsByType");
    if (arr) *count = *(uint32_t *)((char *)arr + 24);
    return arr;
}

// BRINK's renderers are separate from its tracking, colliders and wrist UI.
// Suppress only the named HandModelRenderer through Unity's rendering API.
// Repeat the lookup after scene changes; never retain a managed object pointer.
static void brink_real_hands(unsigned frame) {
    extern const char *kl_driver_target_name(void);
    if (frame < 600 || frame % 120) return;
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x3635172946605196") ||
        !kl_env_on("KL_BRINK_REAL_HANDS", 1) || !mprobe_init()) return;
    static int resolved;
    static const MethodInfo *set_hidden;
    static void *field;
    static void (*field_get)(void *, void *, void *);
    Il2CppClass *k = find_class("", "BrinkHandVisual");
    if (!k) return;
    if (!resolved) {
        resolved = 1;
        kl_image *img = kl_find_image("libil2cpp.so");
        void *(*get_field)(Il2CppClass *, const char *) =
            (void *)kl_sym(img, "il2cpp_class_get_field_from_name");
        field_get = (void *)kl_sym(img, "il2cpp_field_get_value");
        if (get_field) field = get_field(k, "HandModelRenderer");
        set_hidden = find_method("UnityEngine", "Renderer", "set_forceRenderingOff", 1);
        if (!field || !field_get || !set_hidden)
            fprintf(stderr, "  [brink] hand mesh API unavailable; leaving graphics unchanged\n");
    }
    if (!field || !field_get || !set_hidden) return;
    Il2CppObject *type = il.type_get_object(il.class_get_type(k));
    uint32_t n = 0;
    Il2CppObject *arr = mprobe_objects(type, &n);
    if (!arr || n > 16) return;
    for (uint32_t i=0; i<n; i++) {
        Il2CppObject *obj = ((Il2CppObject **)((char *)arr + ARR_DATA))[i];
        Il2CppObject *renderer = NULL, *exc = NULL;
        if (!obj) continue;
        field_get(obj, field, &renderer);
        if (!renderer) continue;
        uint8_t yes = 1; void *args[] = { &yes };
        il.runtime_invoke(set_hidden, renderer, args, &exc);
        static unsigned reported;
        if (!exc && reported < 2) {
            reported++;
            fprintf(stderr, "  [brink] hand mesh %u hidden; tracking and wrist UI retained\n", i);
        }
    }
}

// Anchor the COMPLETE hand menu (grab icon, button tray, and colliders) to
// the matching tracked hand, not the virtual Touch controller's grip offset.
// OpenXR's dorsal +Y remains +Y in Unity. Measure the palm from the skeleton;
// never offset the tracked joints themselves or repeatedly add a displacement.
static void brink_wrist_ui(unsigned frame) {
    extern const char *kl_driver_target_name(void);
    if (frame < 600 || frame % 120) return;
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "x3635172946605196") || !mprobe_init()) return;
    float height = kl_env_float("KL_BRINK_UI_HEIGHT", 0.05f);
    if (height <= 0 || height > 0.12f) return;
    Il2CppClass *dk = find_class("UnityEngine.XR.Hands", "XRHandSkeletonDriver");
    Il2CppClass *hk = find_class("", "BrinkHand");
    if (!dk || !hk) return;
    static int resolved;
    static void *jf, *mf, *ef, *grab_f, *icon_f;
    static void (*fg)(void *, void *, void *);
    static const MethodInfo *root_m, *events_m, *side_m, *tr_m, *id_m;
    static const MethodInfo *pos_m, *up_m, *parent_m, *setpos_m;
    if (!resolved) {
        resolved = 1;
        kl_image *img = kl_find_image("libil2cpp.so");
        void *(*field)(Il2CppClass *, const char *) = (void *)kl_sym(img, "il2cpp_class_get_field_from_name");
        fg = (void *)kl_sym(img, "il2cpp_field_get_value");
        if (field) {
            jf=field(dk,"m_JointTransforms"); mf=field(hk,"Menu"); ef=field(hk,"HandTrackingEvents");
            Il2CppClass *mk=find_class("","BrinkHandMenu"), *ik=find_class("","BrinkHandIcon");
            if(mk) grab_f=field(mk,"GrabIcon");
            if(ik) icon_f=field(ik,"Icon");
        }
        root_m=find_method("UnityEngine.XR.Hands","XRHandSkeletonDriver","get_rootTransform",0);
        events_m=find_method("UnityEngine.XR.Hands","XRHandSkeletonDriver","get_handTrackingEvents",0);
        side_m=find_method("UnityEngine.XR.Hands","XRHandTrackingEvents","get_handedness",0);
        tr_m=find_method("UnityEngine","Component","get_transform",0);
        id_m=find_method("UnityEngine","Object","GetInstanceID",0);
        pos_m=find_method("UnityEngine","Transform","get_position",0);
        up_m=find_method("UnityEngine","Transform","get_up",0);
        parent_m=find_method("UnityEngine","Transform","SetParent",2);
        setpos_m=find_method("UnityEngine","Transform","set_position",1);
    }
    if (!jf||!mf||!ef||!fg||!root_m||!events_m||!side_m||!tr_m||!id_m||!pos_m||!up_m||!parent_m||!setpos_m) return;
    Il2CppObject *anchors[2]={0}; float centers[2][3], normals[2][3];
    uint32_t n=0;
    Il2CppObject *drivers=mprobe_objects(il.type_get_object(il.class_get_type(dk)),&n);
    if (!drivers || n>16) return;
    for (uint32_t i=0;i<n;i++) {
        Il2CppObject *d=((Il2CppObject **)((char *)drivers+ARR_DATA))[i];
        Il2CppObject *events=invoke_on(d,events_m,NULL,"tracked hand events");
        if (!events) continue;
        Il2CppObject *side=invoke_on(events,side_m,NULL,"tracked hand side");
        if (!side) continue;
        int h=*(int *)il.object_unbox(side)-1;
        if (h<0||h>1) continue;
        Il2CppObject *root=invoke_on(d,root_m,NULL,"tracked wrist");
        Il2CppObject *joints=NULL; fg(d,jf,&joints);
        if (!root||!joints||il.array_length(joints)!=26) continue;
        Il2CppObject **js=(Il2CppObject **)((char *)joints+ARR_DATA);
        // Unity XRHandJointID.ToIndex(): wrist 0, palm 1; middle metacarpal 11,
        // proximal 12. Prefer the real palm joint, else its anatomical midpoint.
        Il2CppObject *p0=js[1]?js[1]:js[11], *p1=js[1]?js[1]:js[12];
        if (!p0||!p1) continue;
        Il2CppObject *v=invoke_on(p0,pos_m,NULL,"palm position");
        if (!v) continue;
        float a[3]; memcpy(a,il.object_unbox(v),sizeof a);
        v=invoke_on(p1,pos_m,NULL,"palm position");
        if (!v) continue;
        float *b=il.object_unbox(v);
        for(int c=0;c<3;c++) centers[h][c]=(a[c]+b[c])*0.5f;
        v=invoke_on(root,up_m,NULL,"hand dorsal normal");
        if (!v) continue;
        memcpy(normals[h],il.object_unbox(v),sizeof normals[h]);
        anchors[h]=root;
    }
    static int done[64]; static unsigned used;
    Il2CppObject *hands=mprobe_objects(il.type_get_object(il.class_get_type(hk)),&n);
    if (!hands||n>16) return;
    for(uint32_t i=0;i<n;i++) {
        Il2CppObject *hand=((Il2CppObject **)((char *)hands+ARR_DATA))[i], *menu=NULL,*events=NULL;
        fg(hand,mf,&menu); fg(hand,ef,&events);
        if(!menu||!events) continue;
        Il2CppObject *side=invoke_on(events,side_m,NULL,"menu hand side");
        if(!side) continue;
        int h=*(int *)il.object_unbox(side)-1;
        if(h<0||h>1||!anchors[h]) continue;
        Il2CppObject *tr=invoke_on(menu,tr_m,NULL,"complete hand menu");
        if(!tr) continue;
        Il2CppObject *idobj=invoke_on(tr,id_m,NULL,"menu id");
        if(!idobj) continue;
        int id=*(int *)il.object_unbox(idobj), seen=0;
        for(unsigned j=0;j<used && j<64;j++) if(done[j]==id) seen=1;
        if(seen) continue;
        // Align the visible grab/tool icon itself, including its authored
        // offset inside the complete menu. This also brings every sibling
        // button and interaction collider along by exactly the same amount.
        float root_minus_icon[3]={0};
        Il2CppObject *grab=NULL,*renderer=NULL;
        if(grab_f&&icon_f) {
            fg(menu,grab_f,&grab);
            if(grab) fg(grab,icon_f,&renderer);
        }
        if(renderer) {
            Il2CppObject *icontr=invoke_on(renderer,tr_m,NULL,"visible wrist icon");
            Il2CppObject *rp=invoke_on(tr,pos_m,NULL,"menu world position");
            if(icontr&&rp) {
                memcpy(root_minus_icon,il.object_unbox(rp),sizeof root_minus_icon);
                Il2CppObject *ip=invoke_on(icontr,pos_m,NULL,"visible icon world position");
                if(ip) { float *v=il.object_unbox(ip); for(int c=0;c<3;c++) root_minus_icon[c]-=v[c]; }
                else memset(root_minus_icon,0,sizeof root_minus_icon);
            }
        }
        uint8_t keep_world=1; void *pa[]={anchors[h],&keep_world}; Il2CppObject *exc=NULL;
        il.runtime_invoke(parent_m,tr,pa,&exc);
        if(exc) continue;
        float target[3]; for(int c=0;c<3;c++) target[c]=centers[h][c]+height*normals[h][c]+root_minus_icon[c];
        void *args[]={target}; il.runtime_invoke(setpos_m,tr,args,&exc);
        if(exc) continue;
        done[used%64]=id; used++;
        fprintf(stderr,"  [brink] %s complete menu %d anchored above tracked palm by %.3fm; grab icon and button hitboxes included\n",h?"right":"left",id,height);
    }
}

// KL_PROBE_CALL=<Namespace.Class>::<Method> — call one of the guest's own
// parameterless methods, once, at frame KL_PROBE_CALL_FROM (default 3000).
//
// For a guest that carries its own test driver: Walkabout Mini Golf ships its
// developers' (Mighty.Tester.MightyTestManager, with sequences that load and
// walk every course), and starting one is how every course's pipelines are
// seen without a person playing them. A static method is called as it is; for
// an instance method the object is the class's `Instance` (get_Instance), else
// the first one in the loaded scenes, else — with KL_PROBE_CALL_CREATE=1 — a
// new GameObject that is given the component.
//
// `nth` >= 0 (a KL_PROBE_SCRIPT step with `#n`) calls it on the n-th object
// FindObjectsOfType returns instead, and says the object's name — for a class
// a scene has several of (the six launchers on a wall).
static void mprobe_call_spec(unsigned frame, const char *spec, int nth) {
    if (!spec || !*spec) return;
    char buf[300];
    snprintf(buf, sizeof buf, "%s", spec);
    char *sep = strstr(buf, "::");
    if (!sep) { fprintf(stderr, "  [mprobe] call: '%s' is not Class::Method\n", spec); return; }
    *sep = 0;
    char *meth = sep + 2;
    // Method("text",true,3): up to four arguments — a quoted one is a string,
    // true/false and whole numbers go as the value they are. (A script step's
    // arguments: how a guest's own loader is told WHICH scene.)
    void *argv[4];
    static int32_t ival[4];
    int argc = 0;
    char *paren = strchr(meth, '(');
    if (paren) {
        *paren++ = 0;
        char *close = strrchr(paren, ')');
        if (close) *close = 0;
        for (char *tok = paren; tok && *tok && argc < 4; ) {
            char *next = strchr(tok, ',');
            if (next) *next++ = 0;
            if (*tok == '"') {
                tok++;
                char *q = strrchr(tok, '"');
                if (q) *q = 0;
                argv[argc] = il.string_new(tok);
            } else {
                ival[argc] = !strcmp(tok, "true") ? 1 : !strcmp(tok, "false") ? 0 : (int32_t)strtol(tok, NULL, 0);
                argv[argc] = &ival[argc];
            }
            argc++;
            tok = next;
        }
    }
    char *dot = strrchr(buf, '.');
    const char *ns = "", *cls = buf;
    if (dot) { *dot = 0; ns = buf; cls = dot + 1; }
    Il2CppClass *k = find_class(ns, cls);
    if (!k) { fprintf(stderr, "  [mprobe f%u] call: no class %s.%s\n", frame, ns, cls); return; }
    const MethodInfo *m = il.class_get_method_from_name(k, meth, argc);
    if (!m) { fprintf(stderr, "  [mprobe f%u] call: %s has no %s with %d argument(s)\n", frame, cls, meth, argc); return; }
    kl_image *img = kl_find_image("libil2cpp.so");
    typedef uint32_t (*m_flags_fn)(const MethodInfo *, uint32_t *);
    m_flags_fn m_flags = img ? (m_flags_fn)kl_sym(img, "il2cpp_method_get_flags") : NULL;
    int is_static = m_flags && (m_flags(m, NULL) & 0x10);
    Il2CppObject *self = NULL;
    const char *how = "static";
    char named[160];
    if (!is_static) {
        const MethodInfo *gi = nth < 0 ? il.class_get_method_from_name(k, "get_Instance", 0) : NULL;
        if (gi) { self = invoke(gi, NULL, "get_Instance"); how = "its Instance"; }
        Il2CppObject *type = il.type_get_object(il.class_get_type(k));
        if (nth >= 0) {
            uint32_t count;
            Il2CppObject *arr = mprobe_objects(type, &count);
            if ((uint32_t)nth >= count) {
                fprintf(stderr, "  [mprobe f%u] call: %s #%d — there are %u\n", frame, cls, nth, count);
                return;
            }
            self = ((Il2CppObject **)((char *)arr + 32))[nth];
            const MethodInfo *gn = find_method("UnityEngine", "Object", "get_name", 0);
            char nm[96] = "?";
            if (self && gn) str_ascii(invoke_on(self, gn, NULL, "Object.get_name"), nm, sizeof nm);
            snprintf(named, sizeof named, "#%d of %u, '%s'", nth, count, nm);
            how = named;
        }
        if (!self && type && nth < 0) {
            uint32_t count;
            Il2CppObject *arr = mprobe_objects(type, &count);
            if (count) self = ((Il2CppObject **)((char *)arr + 32))[0];
            snprintf(named, sizeof named, "the first of %u in the scenes", count);
            how = named;
        }
        if (!self && type && nth < 0 && kl_env_on("KL_PROBE_CALL_CREATE", 0)) {
            // new GameObject(name).AddComponent(type)
            Il2CppClass *go_k = find_class("UnityEngine", "GameObject");
            typedef Il2CppObject *(*object_new_fn)(Il2CppClass *);
            object_new_fn object_new = img ? (object_new_fn)kl_sym(img, "il2cpp_object_new") : NULL;
            const MethodInfo *ctor = go_k ? il.class_get_method_from_name(go_k, ".ctor", 1) : NULL;
            const MethodInfo *add  = go_k ? il.class_get_method_from_name(go_k, "AddComponent", 1) : NULL;
            if (object_new && ctor && add) {
                Il2CppObject *go = object_new(go_k);
                void *a1[1] = { il.string_new("KleptonProbe") };
                invoke_on(go, ctor, a1, "GameObject..ctor");
                void *a2[1] = { type };
                self = invoke_on(go, add, a2, "GameObject.AddComponent");
                how = "a component on a new GameObject";
            }
        }
        if (!self) {
            fprintf(stderr, "  [mprobe f%u] call: no %s object to call %s() on%s\n", frame, cls, meth,
                    kl_env_on("KL_PROBE_CALL_CREATE", 0) ? "" : " (KL_PROBE_CALL_CREATE=1 makes one)");
            return;
        }
    }
    fprintf(stderr, "  [mprobe f%u] call: %s.%s::%s(%d) on %s\n", frame, ns, cls, meth, argc, how);
    Il2CppObject *ret = invoke_on(self, m, argc ? argv : NULL, meth);
    // What came back, when it is something small: a getter is then a reading.
    char val[128] = "";
    if (ret) {
        Il2CppClass *rk = il.object_get_class(ret);
        const char *rn = rk ? il.class_get_name(rk) : "?";
        const char *b = (const char *)ret + 16;      // a boxed value's payload
        if (!strcmp(rn, "Boolean")) snprintf(val, sizeof val, " -> %s", *b ? "true" : "false");
        else if (!strcmp(rn, "Int32")) snprintf(val, sizeof val, " -> %d", *(const int32_t *)b);
        else if (!strcmp(rn, "Single")) snprintf(val, sizeof val, " -> %g", (double)*(const float *)b);
        else if (!strcmp(rn, "String")) { char t[100]; str_ascii(ret, t, sizeof t); snprintf(val, sizeof val, " -> \"%s\"", t); }
        else if (!strcmp(rn, "Vector3")) snprintf(val, sizeof val, " -> (%.3f, %.3f, %.3f)",
                                                  (double)((const float *)b)[0], (double)((const float *)b)[1],
                                                  (double)((const float *)b)[2]);
        else snprintf(val, sizeof val, " -> a %s", rn);
    }
    fprintf(stderr, "  [mprobe f%u] call: %s() returned%s\n", frame, meth, val);
}

static void mprobe_call(unsigned frame) { mprobe_call_spec(frame, kl_env_str("KL_PROBE_CALL", NULL), -1); }

// KL_PROBE_SCRIPT=<frame>:<Namespace.Class>::<Method>[(args)][#n];<frame>:... — several
// such calls, each at its frame (rising). For content a guest's own test driver
// does not reach: Walkabout's Homestar Runner basement is entered with
// HomestarManager::GoToBasement and each mini-game in it with HSLauncher::Launch
// on one of the launchers there, `#n` saying which.
static void mprobe_script(unsigned frame) {
    static const char *at;
    static unsigned next;
    static int done;
    if (done) return;
    if (!at) {
        at = kl_env_str("KL_PROBE_SCRIPT", NULL);
        if (!at || !*at) { done = 1; return; }
        next = (unsigned)strtoul(at, NULL, 10);
    }
    while (frame >= next) {
        const char *colon = strchr(at, ':');
        const char *end = strchr(at, ';');
        if (!end) end = at + strlen(at);
        if (!colon || colon > end) { fprintf(stderr, "  [mprobe] script: '%s' is not <frame>:Class::Method\n", at); done = 1; return; }
        char step[300];
        size_t n = (size_t)(end - colon - 1);
        if (n >= sizeof step) n = sizeof step - 1;
        memcpy(step, colon + 1, n);
        step[n] = 0;
        int nth = -1;
        char *hash = strrchr(step, '#');
        if (hash) { *hash = 0; nth = atoi(hash + 1); }
        if (mprobe_init()) mprobe_call_spec(frame, step, nth);
        if (!*end) { done = 1; return; }
        at = end + 1;
        next = (unsigned)strtoul(at, NULL, 10);
    }
}

// ---------------------------------------------------------------------------
// Two more for a guest that carries its own test driver, both on a field of the
// object KL_PROBE_CALL would call (the class's Instance):
//
// KL_PROBE_KEEP=<Namespace.Class>::<field>=<part>[|<part>...] — the field is a
// List of Unity objects; keep the ones whose name contains one of the parts
// (case does not matter) and drop the rest, once, just before KL_PROBE_CALL.
// Walkabout's test manager walks the courses in `coursesToTest`; with this it
// walks the chosen ones. An empty part list only prints the names.
//
// KL_PROBE_STRINGS=<Namespace.Class>::<field> — the field is a List of strings
// (a test driver's results); new entries are printed as they appear.
//
// A List<T> is { header, T[] _items, int _size, int _version }: _items at 16,
// _size at 24, and an array's elements at 32 — the layout KL_PROBE_ENUM and the
// rest of this file already rely on.
static Il2CppObject *mprobe_instance_field(const char *spec, char *rest, size_t rest_cap, const char **why) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s", spec);
    char *eq = strchr(buf, '=');
    if (rest) snprintf(rest, rest_cap, "%s", eq ? eq + 1 : "");
    if (eq) *eq = 0;
    char *sep = strstr(buf, "::");
    if (!sep) { *why = "not Class::field"; return NULL; }
    *sep = 0;
    const char *field = sep + 2;
    char *dot = strrchr(buf, '.');
    const char *ns = "", *cls = buf;
    if (dot) { *dot = 0; ns = buf; cls = dot + 1; }
    Il2CppClass *k = find_class(ns, cls);
    if (!k) { *why = "no such class"; return NULL; }
    const MethodInfo *gi = il.class_get_method_from_name(k, "get_Instance", 0);
    Il2CppObject *self = gi ? invoke(gi, NULL, "get_Instance") : NULL;
    if (!self) { *why = "the class has no Instance (yet)"; return NULL; }
    kl_image *img = kl_find_image("libil2cpp.so");
    typedef void *(*field_from_name_fn)(Il2CppClass *, const char *);
    typedef size_t (*f_off_fn)(void *);
    field_from_name_fn ffn = img ? (field_from_name_fn)kl_sym(img, "il2cpp_class_get_field_from_name") : NULL;
    f_off_fn f_off = img ? (f_off_fn)kl_sym(img, "il2cpp_field_get_offset") : NULL;
    void *f = ffn ? ffn(k, field) : NULL;
    if (!f || !f_off) { *why = "no such field"; return NULL; }
    Il2CppObject *list = *(Il2CppObject **)((uint8_t *)self + f_off(f));
    if (!list) *why = "the field is null";
    return list;
}

static void mprobe_keep(unsigned frame) {
    const char *spec = kl_env_str("KL_PROBE_KEEP", NULL);
    if (!spec || !*spec) return;
    char parts[400];
    const char *why = "";
    Il2CppObject *list = mprobe_instance_field(spec, parts, sizeof parts, &why);
    if (!list) { fprintf(stderr, "  [mprobe f%u] keep %s: %s\n", frame, spec, why); return; }
    Il2CppObject *items = *(Il2CppObject **)((uint8_t *)list + 16);
    int32_t *size = (int32_t *)((uint8_t *)list + 24);
    const MethodInfo *get_name = find_method("UnityEngine", "Object", "get_name", 0);
    if (!items || !get_name || *size < 0 || (uint32_t)*size > il.array_length(items)) {
        fprintf(stderr, "  [mprobe f%u] keep %s: not a list of Unity objects\n", frame, spec);
        return;
    }
    const char *want[64]; int nw = 0;
    for (char *t = strtok(parts, "|"); t && nw < 64; t = strtok(NULL, "|")) want[nw++] = t;
    // KL_PROBE_KEEP_FIELD=<name>: a string field of the elements that is matched
    // as well, and then as a whole — Walkabout's courses carry the name of their
    // scene (SceneName), which is what its files are called after.
    const char *also = kl_env_str("KL_PROBE_KEEP_FIELD", NULL);
    kl_image *img = kl_find_image("libil2cpp.so");
    typedef void *(*field_from_name_fn)(Il2CppClass *, const char *);
    typedef size_t (*f_off_fn)(void *);
    field_from_name_fn ffn = img ? (field_from_name_fn)kl_sym(img, "il2cpp_class_get_field_from_name") : NULL;
    f_off_fn f_off = img ? (f_off_fn)kl_sym(img, "il2cpp_field_get_offset") : NULL;
    Il2CppObject **el = (Il2CppObject **)((uint8_t *)items + ARR_DATA);
    int kept = 0, n = *size;
    for (int i = 0; i < n; i++) {
        char name[160] = "(null)", extra[160] = "";
        if (el[i]) str_ascii(invoke_on(el[i], get_name, NULL, "Object.get_name"), name, sizeof name);
        if (el[i] && also && *also && ffn && f_off) {
            void *f = ffn(il.object_get_class(el[i]), also);
            Il2CppObject *v = f ? *(Il2CppObject **)((uint8_t *)el[i] + f_off(f)) : NULL;
            if (v) str_ascii(v, extra, sizeof extra);
        }
        int hit = nw == 0;
        for (int w = 0; w < nw && !hit; w++)
            hit = strcasestr(name, want[w]) != NULL || (extra[0] && strcasecmp(extra, want[w]) == 0);
        fprintf(stderr, "  [mprobe f%u] keep: %2d %-34s %-34s %s\n", frame, i, name, extra, nw == 0 ? "" : hit ? "kept" : "dropped");
        if (hit) el[kept++] = el[i];
    }
    if (nw) {
        for (int i = kept; i < n; i++) el[i] = NULL;
        *size = kept;
        (*(int32_t *)((uint8_t *)list + 28))++;          // _version: an enumeration in flight notices
        fprintf(stderr, "  [mprobe f%u] keep %s: %d of %d left\n", frame, spec, kept, n);
    }
}

static void mprobe_strings(unsigned frame) {
    const char *spec = kl_env_str("KL_PROBE_STRINGS", NULL);
    if (!spec || !*spec) return;
    static int seen;
    const char *why = "";
    Il2CppObject *list = mprobe_instance_field(spec, NULL, 0, &why);
    if (!list) return;
    Il2CppObject *items = *(Il2CppObject **)((uint8_t *)list + 16);
    int32_t n = *(int32_t *)((uint8_t *)list + 24);
    if (!items || n < 0 || (uint32_t)n > il.array_length(items)) return;
    if (n < seen) seen = 0;                               // the list was cleared
    Il2CppObject **el = (Il2CppObject **)((uint8_t *)items + ARR_DATA);
    for (; seen < n; seen++) {
        char text[700];
        str_ascii(el[seen], text, sizeof text);
        fprintf(stderr, "  [mprobe f%u] %s[%d]: %s\n", frame, spec, seen, text);
    }
}

// ---- AC Nexus VR: the frame length the guest spins itself out to -------------
//
// Not a probe: the one place the runtime writes a managed value, kept here
// because the il2cpp bindings are. AC Nexus runs with Application SpaceWarp and
// paces ITSELF for it: NexusUtils.PhaseSyncHack.SpinWaitAtEndOfNexusFrame spins
// every frame out to _nexusFrameTicksSpaceWarp, which its Awake sets from a
// per-headset constant — 85% of two display periods, 21.25 ms for a Quest 2 at
// 80 Hz (Q2_TARGET) and 18.89 ms for a Quest 3 at 90 Hz (Q3_TARGET). On a Quest
// the compositor then holds the app to half the display rate and synthesises
// the frames between. Nothing synthesises frames here, so that spin is half the
// frame rate given away for nothing, and at the Quest 2 length it is not even a
// whole number of display frames: 46.7 fps against a 100 Hz display, six frames
// shown for two periods and the seventh for three.
//
// So the spin is taken out (0 ticks, the value the game itself uses outside
// SpaceWarp — _nexusFrameTicksNormal is 0) and the game runs as fast as it
// can: 100 fps in its menus and 70-98 in a level on device, where the spin had
// held both at 46.7. Only the value its Awake wrote for a Quest 2 is replaced.
// KL_NEXUS_FRAME_BUDGET=0 leaves it alone; KL_NEXUS_FRAME_BUDGET_MS=<ms> spins
// to that length instead (17.0 is the game's own rule at 100 Hz, and with
// KL_GUEST_FRESH_POSE gives exactly half the display rate).
void kl_mprobe_nexus_frame_budget(unsigned frame) {
    static int on = -1;
    static double ms;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_NEXUS_FRAME_BUDGET", t && strcmp(t, "x5812519008825194") == 0);
        const char *e = kl_env_str("KL_NEXUS_FRAME_BUDGET_MS", NULL);
        ms = e ? strtod(e, NULL) : 0.0;
        if (!(ms >= 0.0 && ms < 21.25)) ms = 0.0;
    }
    if (!on || frame < 300 || frame % 300) return;
    if (!mprobe_init()) return;
    static const MethodInfo *m_fo; static Il2CppObject *type; static size_t off; static int r;
    if (!r) {
        r = 1;
        kl_image *img = kl_find_image("libil2cpp.so");
        Il2CppClass *k = find_class("NexusUtils", "PhaseSyncHack");
        typedef void *(*gfn_fn)(Il2CppClass *, const char *);
        typedef size_t (*f_off_fn)(void *);
        gfn_fn gfn = img ? (gfn_fn)kl_sym(img, "il2cpp_class_get_field_from_name") : NULL;
        f_off_fn fo = img ? (f_off_fn)kl_sym(img, "il2cpp_field_get_offset") : NULL;
        void *f = k && gfn ? gfn(k, "_nexusFrameTicksSpaceWarp") : NULL;
        if (f && fo) off = fo(f);
        if (k) type = il.type_get_object(il.class_get_type(k));
        m_fo = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        if (!off || !type || !m_fo) {
            fprintf(stderr, "  [nexus] frame length: PhaseSyncHack not found as expected — left alone\n");
            on = 0;
            return;
        }
    }
    int64_t want = (int64_t)(ms * 1e4);                       // 100 ns ticks
    void *ta[1] = { type };
    Il2CppObject *arr = invoke(m_fo, ta, "FindObjectsOfType(PhaseSyncHack)");
    unsigned n = arr ? il.array_length(arr) : 0;
    for (unsigned i = 0; i < n; i++) {
        Il2CppObject *o = ((Il2CppObject **)((uint8_t *)arr + ARR_DATA))[i];
        if (!o) continue;
        int64_t *p = (int64_t *)((uint8_t *)o + off);
        if (*p == 212500) {                                   // Q2_TARGET, as Awake wrote it
            fprintf(stderr, "  [nexus] frame length: the game's SpaceWarp spin of %.2f ms "
                            "(Quest 2 at 80 Hz) -> %.2f ms\n", (double)*p / 1e4, ms);
            *p = want;
        }
    }
}

// KL_PROBE_LINES=1 — every KL_PROBE_LINES_EVERY frames (default 300) from frame
// 600: each LineRenderer the game has switched on, by name with its first
// three ancestors, whether it renders and is visible, its point count, first
// and last point (and whether those are world space), start colour and width.
// For "a green line across the play area that does not move" (Synth Riders,
// 2026-10-15): which object it is, read off the running scene instead of
// guessed. Diagnostic only, off unless asked for.
static void probe_lines(unsigned frame) {
    static const MethodInfo *m_find, *m_name, *m_tf, *m_parent, *m_en, *m_vis,
                            *m_cnt, *m_pos, *m_ws, *m_col, *m_w, *m_tp, *m_cam, *m_wpos;
    static Il2CppObject *lr_type;
    static Il2CppClass *v3_class;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_find   = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_parent = find_method("UnityEngine", "Transform", "get_parent", 0);
        m_en     = find_method("UnityEngine", "Renderer", "get_enabled", 0);
        m_vis    = find_method("UnityEngine", "Renderer", "get_isVisible", 0);
        m_cnt    = find_method("UnityEngine", "LineRenderer", "get_positionCount", 0);
        m_pos    = find_method("UnityEngine", "LineRenderer", "GetPositions", 1);
        m_ws     = find_method("UnityEngine", "LineRenderer", "get_useWorldSpace", 0);
        m_col    = find_method("UnityEngine", "LineRenderer", "get_startColor", 0);
        m_w      = find_method("UnityEngine", "LineRenderer", "get_widthMultiplier", 0);
        m_tp     = find_method("UnityEngine", "Transform", "TransformPoint", 1);
        m_cam    = find_method("UnityEngine", "Camera", "get_main", 0);
        m_wpos   = find_method("UnityEngine", "Transform", "get_position", 0);
        v3_class = find_class("UnityEngine", "Vector3");
        Il2CppClass *k = find_class("UnityEngine", "LineRenderer");
        if (k) lr_type = il.type_get_object(il.class_get_type(k));
    }
    if (!m_find || !lr_type) return;
    {
        // Where the head is, to read the lines against.
        Il2CppObject *cam = invoke(m_cam, NULL, "Camera.main"), *o, *ct;
        float c[3] = {0};
        if (cam && (ct = invoke_on(cam, m_tf, NULL, "transform")) && (o = invoke_on(ct, m_wpos, NULL, "position")))
            memcpy(c, il.object_unbox(o), sizeof c);
        fprintf(stderr, "  [mprobe f%u] camera at (%.2f %.2f %.2f)\n", frame, (double)c[0], (double)c[1], (double)c[2]);
    }
    void *ta[1] = { lr_type };
    Il2CppObject *arr = invoke(m_find, ta, "FindObjectsOfType(LineRenderer)");
    if (!arr) return;
    int n = (int)il.array_length(arr);
    Il2CppObject **items = (Il2CppObject **)((uint8_t *)arr + 0x20);   // Il2CppArray: elements after header
    fprintf(stderr, "  [mprobe f%u] lines: %d LineRenderer(s) switched on\n", frame, n);
    for (int i = 0; i < n && i < 64; i++) {
        Il2CppObject *lr = items[i];
        if (!lr) continue;
        Il2CppObject *o;
        int en = (o = invoke_on(lr, m_en, NULL, "enabled")) ? *(uint8_t *)il.object_unbox(o) : -1;
        int vis = (o = invoke_on(lr, m_vis, NULL, "isVisible")) ? *(uint8_t *)il.object_unbox(o) : -1;
        int cnt = (o = invoke_on(lr, m_cnt, NULL, "positionCount")) ? *(int32_t *)il.object_unbox(o) : -1;
        int ws = (o = invoke_on(lr, m_ws, NULL, "useWorldSpace")) ? *(uint8_t *)il.object_unbox(o) : -1;
        float col[4] = {0}, w = 0, a[3] = {0}, b[3] = {0}, mid[3] = {0};
        if ((o = invoke_on(lr, m_col, NULL, "startColor"))) memcpy(col, il.object_unbox(o), sizeof col);
        if ((o = invoke_on(lr, m_w, NULL, "widthMultiplier"))) w = *(float *)il.object_unbox(o);
        Il2CppObject *tf0 = invoke_on(lr, m_tf, NULL, "transform");
        if (cnt > 0 && vis == 1 && m_pos && v3_class && il.array_new) {
            Il2CppObject *pa = il.array_new(v3_class, (uintptr_t)cnt);
            void *aa[1] = { pa };
            if (pa && invoke_on(lr, m_pos, aa, "GetPositions")) {
                const float *v = (const float *)((uint8_t *)pa + 0x20);
                memcpy(a, v, sizeof a); memcpy(mid, v + 3 * (cnt / 2), sizeof mid); memcpy(b, v + 3 * (cnt - 1), sizeof b);
                if (ws == 0 && tf0 && m_tp) {                 // local: to world
                    float *pts[3] = { a, mid, b };
                    for (int q = 0; q < 3; q++) {
                        void *ta2[1] = { pts[q] };
                        if ((o = invoke_on(tf0, m_tp, ta2, "TransformPoint"))) memcpy(pts[q], il.object_unbox(o), 12);
                    }
                }
            }
        }
        char chain[256] = "", nm[64];
        Il2CppObject *t = invoke_on(lr, m_tf, NULL, "transform");
        for (int d = 0; t && d < 4; d++) {
            Il2CppObject *s2 = invoke_on(t, m_name, NULL, "name");
            str_ascii(s2, nm, sizeof nm);
            size_t L = strlen(chain);
            snprintf(chain + L, sizeof chain - L, "%s%s", d ? " < " : "", nm);
            t = invoke_on(t, m_parent, NULL, "parent");
        }
        if (vis != 1) continue;
        fprintf(stderr, "  [mprobe f%u]   line %d: %s | on %d visible %d | %d pts %s, world (%.2f %.2f %.2f) .. "
                        "(%.2f %.2f %.2f) .. (%.2f %.2f %.2f) | colour %.2f %.2f %.2f %.2f | width %.3f\n",
                frame, i, chain, en, vis, cnt, ws == 1 ? "world" : ws == 0 ? "local" : "?",
                (double)a[0], (double)a[1], (double)a[2], (double)mid[0], (double)mid[1], (double)mid[2],
                (double)b[0], (double)b[1], (double)b[2],
                (double)col[0], (double)col[1], (double)col[2], (double)col[3], (double)w);
    }
}

// ...and every visible Renderer, of any type, whose world bounds are long and
// thin (one extent over 0.5 m, i.e. over a metre across, and another under
// 2.5 cm): a line on screen that is not a LineRenderer (Synth Riders: none was
// visible while its green line was). Name with three ancestors, the renderer's
// own class, bounds centre and size.
static void probe_thin_renderers(unsigned frame) {
    static const MethodInfo *m_find, *m_name, *m_tf, *m_parent, *m_vis, *m_bounds;
    static Il2CppObject *r_type;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        m_find   = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_parent = find_method("UnityEngine", "Transform", "get_parent", 0);
        m_vis    = find_method("UnityEngine", "Renderer", "get_isVisible", 0);
        m_bounds = find_method("UnityEngine", "Renderer", "get_bounds", 0);
        Il2CppClass *k = find_class("UnityEngine", "Renderer");
        if (k) r_type = il.type_get_object(il.class_get_type(k));
    }
    if (!m_find || !r_type || !m_bounds) return;
    void *ta[1] = { r_type };
    Il2CppObject *arr = invoke(m_find, ta, "FindObjectsOfType(Renderer)");
    if (!arr) return;
    int n = (int)il.array_length(arr), vis_n = 0, thin_n = 0;
    Il2CppObject **items = (Il2CppObject **)((uint8_t *)arr + 0x20);
    for (int i = 0; i < n && i < 4096; i++) {
        Il2CppObject *r = items[i], *o;
        if (!r) continue;
        if (!(o = invoke_on(r, m_vis, NULL, "isVisible")) || !*(uint8_t *)il.object_unbox(o)) continue;
        vis_n++;
        float bb[6] = {0};                                    // Bounds: centre, extents
        if (!(o = invoke_on(r, m_bounds, NULL, "bounds"))) continue;
        memcpy(bb, il.object_unbox(o), sizeof bb);
        float ex[3] = { bb[3], bb[4], bb[5] }, mx = 0, mn = 1e9f;
        for (int k = 0; k < 3; k++) { if (ex[k] > mx) mx = ex[k]; if (ex[k] < mn) mn = ex[k]; }
        if (!(mx > 0.5f && mn < 0.025f)) continue;
        if (thin_n++ >= 20) continue;
        char chain[256] = "", nm[64];
        Il2CppObject *t = invoke_on(r, m_tf, NULL, "transform");
        for (int d = 0; t && d < 4; d++) {
            Il2CppObject *s2 = invoke_on(t, m_name, NULL, "name");
            str_ascii(s2, nm, sizeof nm);
            size_t L = strlen(chain);
            snprintf(chain + L, sizeof chain - L, "%s%s", d ? " < " : "", nm);
            t = invoke_on(t, m_parent, NULL, "parent");
        }
        Il2CppClass *kc = il.object_get_class(r);
        fprintf(stderr, "  [mprobe f%u]   thin %s: %s | centre (%.2f %.2f %.2f) size (%.2f %.2f %.2f)\n",
                frame, kc ? il.class_get_name(kc) : "?", chain,
                (double)bb[0], (double)bb[1], (double)bb[2],
                (double)(2 * ex[0]), (double)(2 * ex[1]), (double)(2 * ex[2]));
    }
    fprintf(stderr, "  [mprobe f%u] renderers: %d switched on, %d visible, %d long and thin\n",
            frame, n, vis_n, thin_n);
}

// ...and every visible Renderer whose material is GREEN — the one thing known
// for sure about Synth Riders' stray line. Its sharedMaterial's _Color,
// _BaseColor, _TintColor, _EmissionColor, _MainColor where the shader has them;
// green: G over 0.5 and over twice R and B. Class, name with ancestors, shader
// name, the property and its value.
static void probe_green_renderers(unsigned frame) {
    static const MethodInfo *m_find, *m_name, *m_tf, *m_parent, *m_vis, *m_mat, *m_has, *m_getc,
                            *m_shader;
    static Il2CppObject *r_type;
    static Il2CppObject *pname[5];
    static int resolved;
    static const char *const props[5] = { "_Color", "_BaseColor", "_TintColor", "_EmissionColor", "_MainColor" };
    if (!resolved) {
        resolved = 1;
        m_find   = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_parent = find_method("UnityEngine", "Transform", "get_parent", 0);
        m_vis    = find_method("UnityEngine", "Renderer", "get_isVisible", 0);
        m_mat    = find_method("UnityEngine", "Renderer", "get_sharedMaterial", 0);
        m_shader = find_method("UnityEngine", "Material", "get_shader", 0);
        Il2CppClass *mk = find_class("UnityEngine", "Material");
        if (mk) {
            // HasProperty(string) and GetColor(string): the overload with a string.
            void *it = NULL; const MethodInfo *mm;
            typedef const MethodInfo *(*gm_fn)(Il2CppClass *, void **);
            typedef const char *(*mn_fn)(const MethodInfo *);
            typedef uint32_t (*pc_fn)(const MethodInfo *);
            typedef void *(*pt_fn)(const MethodInfo *, uint32_t);
            typedef const char *(*tn_fn)(void *);
            kl_image *img = kl_find_image("libil2cpp.so");
            gm_fn gm = (gm_fn)kl_sym(img, "il2cpp_class_get_methods");
            mn_fn mn = (mn_fn)kl_sym(img, "il2cpp_method_get_name");
            pc_fn pc = (pc_fn)kl_sym(img, "il2cpp_method_get_param_count");
            pt_fn pt = (pt_fn)kl_sym(img, "il2cpp_method_get_param");
            tn_fn tn = (tn_fn)kl_sym(img, "il2cpp_type_get_name");
            while (img && gm && mn && pc && pt && tn && (mm = gm(mk, &it))) {
                const char *nm = mn(mm);
                if (pc(mm) != 1) continue;
                const char *ty = tn(pt(mm, 0));
                if (!ty || strcmp(ty, "System.String")) continue;
                if (!strcmp(nm, "HasProperty")) m_has = mm;
                if (!strcmp(nm, "GetColor"))    m_getc = mm;
            }
        }
        for (int i = 0; i < 5; i++) pname[i] = il.string_new(props[i]);
        Il2CppClass *k = find_class("UnityEngine", "Renderer");
        if (k) r_type = il.type_get_object(il.class_get_type(k));
        fprintf(stderr, "  [mprobe] green census: HasProperty %s, GetColor %s\n",
                m_has ? "found" : "MISSING", m_getc ? "found" : "MISSING");
    }
    if (!m_find || !r_type || !m_mat || !m_has || !m_getc) return;
    void *ta[1] = { r_type };
    Il2CppObject *arr = invoke(m_find, ta, "FindObjectsOfType(Renderer)");
    if (!arr) return;
    int n = (int)il.array_length(arr), green_n = 0;
    Il2CppObject **items = (Il2CppObject **)((uint8_t *)arr + 0x20);
    for (int i = 0; i < n && i < 4096; i++) {
        Il2CppObject *r = items[i], *o, *mat;
        if (!r) continue;
        if (!(o = invoke_on(r, m_vis, NULL, "isVisible")) || !*(uint8_t *)il.object_unbox(o)) continue;
        if (!(mat = invoke_on(r, m_mat, NULL, "sharedMaterial"))) continue;
        for (int p = 0; p < 5; p++) {
            void *pa[1] = { pname[p] };
            if (!(o = invoke_on(mat, m_has, pa, "HasProperty")) || !*(uint8_t *)il.object_unbox(o)) continue;
            float c[4] = {0};
            if (!(o = invoke_on(mat, m_getc, pa, "GetColor"))) continue;
            memcpy(c, il.object_unbox(o), sizeof c);
            if (!(c[1] > 0.5f && c[1] > 2 * c[0] && c[1] > 2 * c[2])) continue;
            if (green_n++ >= 20) break;
            char chain[256] = "", nm[64], sh[96] = "?";
            Il2CppObject *t = invoke_on(r, m_tf, NULL, "transform");
            for (int d = 0; t && d < 4; d++) {
                Il2CppObject *s2 = invoke_on(t, m_name, NULL, "name");
                str_ascii(s2, nm, sizeof nm);
                size_t L = strlen(chain);
                snprintf(chain + L, sizeof chain - L, "%s%s", d ? " < " : "", nm);
                t = invoke_on(t, m_parent, NULL, "parent");
            }
            Il2CppObject *shd = invoke_on(mat, m_shader, NULL, "shader");
            if (shd) str_ascii(invoke_on(shd, m_name, NULL, "name"), sh, sizeof sh);
            Il2CppClass *kc = il.object_get_class(r);
            fprintf(stderr, "  [mprobe f%u]   green %s: %s | shader %s | %s %.2f %.2f %.2f %.2f\n",
                    frame, kc ? il.class_get_name(kc) : "?", chain, sh, props[p],
                    (double)c[0], (double)c[1], (double)c[2], (double)c[3]);
            break;
        }
    }
    fprintf(stderr, "  [mprobe f%u] green renderers: %d\n", frame, green_n);
}

// A method of `k` named `name` whose `argc` parameters' first is of type `ty0`.
static const MethodInfo *find_method_typed(Il2CppClass *k, const char *name, uint32_t argc, const char *ty0);
static const MethodInfo *find_method_str(Il2CppClass *k, const char *name, uint32_t argc) {
    return find_method_typed(k, name, argc, "System.String");
}
static const MethodInfo *find_method_typed(Il2CppClass *k, const char *name, uint32_t argc, const char *ty0) {
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img || !k) return NULL;
    typedef const MethodInfo *(*gm_fn)(Il2CppClass *, void **);
    typedef const char *(*mn_fn)(const MethodInfo *);
    typedef uint32_t (*pc_fn)(const MethodInfo *);
    typedef void *(*pt_fn)(const MethodInfo *, uint32_t);
    typedef const char *(*tn_fn)(void *);
    gm_fn gm = (gm_fn)kl_sym(img, "il2cpp_class_get_methods");
    mn_fn mn = (mn_fn)kl_sym(img, "il2cpp_method_get_name");
    pc_fn pc = (pc_fn)kl_sym(img, "il2cpp_method_get_param_count");
    pt_fn pt = (pt_fn)kl_sym(img, "il2cpp_method_get_param");
    tn_fn tn = (tn_fn)kl_sym(img, "il2cpp_type_get_name");
    void *it = NULL; const MethodInfo *mm;
    while (gm && mn && pc && pt && tn && (mm = gm(k, &it))) {
        if (pc(mm) != argc || strcmp(mn(mm), name)) continue;
        const char *ty = tn(pt(mm, 0));
        if (ty && !strcmp(ty, ty0)) return mm;
    }
    return NULL;
}

// KL_PROBE_RAILS: around song start (frames 800..1400, every 15), the visible
// rail LineRenderers by name, counted — how long Synth Riders' rails show at
// the start of a song — and, once, every property of the first visible green
// ("One Hand") rail's material with its value: whatever hides a pooled rail on
// a Quest is one of these. Lighter than KL_PROBE_LINES (no renderer census).
static void probe_rails(unsigned frame) {
    static const MethodInfo *m_find, *m_name, *m_vis, *m_mat, *m_shader, *m_pcount, *m_pname, *m_ptype,
                            *m_getf, *m_getv, *m_getpos, *m_cnt, *m_ws, *m_tf, *m_tp, *m_wpos, *m_cam, *m_gofind;
    static Il2CppClass *v3k;
    static Il2CppObject *lr_type;
    static int resolved, dumped;
    if (!resolved) {
        resolved = 1;
        m_find   = find_method("UnityEngine", "Object", "FindObjectsOfType", 1);
        m_name   = find_method("UnityEngine", "Object", "get_name", 0);
        m_vis    = find_method("UnityEngine", "Renderer", "get_isVisible", 0);
        m_mat    = find_method("UnityEngine", "Renderer", "get_sharedMaterial", 0);
        m_shader = find_method("UnityEngine", "Material", "get_shader", 0);
        m_pcount = find_method("UnityEngine", "Shader", "GetPropertyCount", 0);
        m_pname  = find_method("UnityEngine", "Shader", "GetPropertyName", 1);
        m_ptype  = find_method("UnityEngine", "Shader", "GetPropertyType", 1);
        Il2CppClass *mk = find_class("UnityEngine", "Material");
        m_getf   = find_method_str(mk, "GetFloat", 1);
        m_getv   = find_method_str(mk, "GetVector", 1);
        m_getpos = find_method_typed(find_class("UnityEngine", "LineRenderer"), "GetPositions", 1, "UnityEngine.Vector3[]");
        m_cnt    = find_method("UnityEngine", "LineRenderer", "get_positionCount", 0);
        m_ws     = find_method("UnityEngine", "LineRenderer", "get_useWorldSpace", 0);
        m_tf     = find_method("UnityEngine", "Component", "get_transform", 0);
        m_tp     = find_method("UnityEngine", "Transform", "TransformPoint", 1);
        m_wpos   = find_method("UnityEngine", "Transform", "get_position", 0);
        m_cam    = find_method("UnityEngine", "Camera", "get_main", 0);
        m_gofind = find_method("UnityEngine", "GameObject", "Find", 1);
        v3k      = find_class("UnityEngine", "Vector3");
        fprintf(stderr, "  [mprobe] rails: GetPositions(Vector3[]) %s\n", m_getpos ? "found" : "MISSING");
        Il2CppClass *k = find_class("UnityEngine", "LineRenderer");
        if (k) lr_type = il.type_get_object(il.class_get_type(k));
    }
    if (!m_find || !lr_type) return;
    void *ta[1] = { lr_type };
    Il2CppObject *arr = invoke(m_find, ta, "FindObjectsOfType(LineRenderer)");
    if (!arr) return;
    int n = (int)il.array_length(arr), one = 0, two = 0, left = 0, right = 0, other = 0, shown = 0;
    Il2CppObject **items = (Il2CppObject **)((uint8_t *)arr + 0x20);
    // Where the head and the hands are, to read the rails against.
    {
        Il2CppObject *o, *t;
        float c[3] = {0}, l[3] = {0}, r[3] = {0};
        Il2CppObject *cam = invoke(m_cam, NULL, "Camera.main");
        if (cam && (t = invoke_on(cam, m_tf, NULL, "transform")) && (o = invoke_on(t, m_wpos, NULL, "position")))
            memcpy(c, il.object_unbox(o), 12);
        const char *names[2] = { "LeftHand", "RightHand" }; float *dst[2] = { l, r };
        for (int h = 0; h < 2 && m_gofind; h++) {
            void *fa[1] = { il.string_new(names[h]) };
            Il2CppObject *go = invoke(m_gofind, fa, "GameObject.Find");
            static const MethodInfo *m_got;
            if (!m_got) m_got = find_method("UnityEngine", "GameObject", "get_transform", 0);
            if (go && (t = invoke_on(go, m_got, NULL, "transform")) && (o = invoke_on(t, m_wpos, NULL, "position")))
                memcpy(dst[h], il.object_unbox(o), 12);
        }
        fprintf(stderr, "  [mprobe f%u] head (%.2f %.2f %.2f) left hand (%.2f %.2f %.2f) right hand (%.2f %.2f %.2f)\n",
                frame, (double)c[0], (double)c[1], (double)c[2], (double)l[0], (double)l[1], (double)l[2],
                (double)r[0], (double)r[1], (double)r[2]);
    }
    for (int i = 0; i < n && i < 256; i++) {
        Il2CppObject *lr = items[i], *o;
        if (!lr || !(o = invoke_on(lr, m_vis, NULL, "isVisible")) || !*(uint8_t *)il.object_unbox(o)) continue;
        char nm[64];
        str_ascii(invoke_on(lr, m_name, NULL, "name"), nm, sizeof nm);
        if (strstr(nm, "One Hand") && shown < 2 && m_getpos && v3k && il.array_new) {
            shown++;
            int cnt = (o = invoke_on(lr, m_cnt, NULL, "positionCount")) ? *(int32_t *)il.object_unbox(o) : 0;
            int ws = (o = invoke_on(lr, m_ws, NULL, "useWorldSpace")) ? *(uint8_t *)il.object_unbox(o) : -1;
            float pts[3][3] = {{0}};
            if (cnt > 0) {
                Il2CppObject *pa = il.array_new(v3k, (uintptr_t)cnt);
                void *aa[1] = { pa };
                if (pa && invoke_on(lr, m_getpos, aa, "GetPositions")) {
                    const float *v = (const float *)((uint8_t *)pa + 0x20);
                    memcpy(pts[0], v, 12); memcpy(pts[1], v + 3 * (cnt / 2), 12); memcpy(pts[2], v + 3 * (cnt - 1), 12);
                    Il2CppObject *t = invoke_on(lr, m_tf, NULL, "transform");
                    for (int q = 0; ws == 0 && t && q < 3; q++) {
                        void *ta2[1] = { pts[q] };
                        if ((o = invoke_on(t, m_tp, ta2, "TransformPoint"))) memcpy(pts[q], il.object_unbox(o), 12);
                    }
                }
            }
            fprintf(stderr, "  [mprobe f%u]   %s: %d pts %s, world (%.2f %.2f %.2f) .. (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)\n",
                    frame, nm, cnt, ws == 1 ? "world" : "local",
                    (double)pts[0][0], (double)pts[0][1], (double)pts[0][2], (double)pts[1][0], (double)pts[1][1],
                    (double)pts[1][2], (double)pts[2][0], (double)pts[2][1], (double)pts[2][2]);
        }
        if (strstr(nm, "One Hand")) one++; else if (strstr(nm, "Two Hand")) two++;
        else if (strstr(nm, "Left Hand")) left++; else if (strstr(nm, "Right Hand")) right++; else other++;
        if (!dumped && strstr(nm, "One Hand") && m_mat && m_shader && m_pcount && m_pname && m_ptype) {
            dumped = 1;
            Il2CppObject *mat = invoke_on(lr, m_mat, NULL, "sharedMaterial"), *shd;
            if (!mat || !(shd = invoke_on(mat, m_shader, NULL, "shader"))) continue;
            char sh[96];
            str_ascii(invoke_on(shd, m_name, NULL, "name"), sh, sizeof sh);
            int pc = (o = invoke_on(shd, m_pcount, NULL, "GetPropertyCount")) ? *(int32_t *)il.object_unbox(o) : 0;
            fprintf(stderr, "  [mprobe f%u] rail material: shader %s, %d properties\n", frame, sh, pc);
            for (int32_t p = 0; p < pc && p < 64; p++) {
                void *pa[1] = { &p };
                Il2CppObject *pn = invoke_on(shd, m_pname, pa, "GetPropertyName");
                int ty = (o = invoke_on(shd, m_ptype, pa, "GetPropertyType")) ? *(int32_t *)il.object_unbox(o) : -1;
                char pnm[64];
                str_ascii(pn, pnm, sizeof pnm);
                void *na[1] = { pn };
                if ((ty == 2 || ty == 3 || ty == 5) && m_getf && (o = invoke_on(mat, m_getf, na, "GetFloat")))
                    fprintf(stderr, "  [mprobe]     %s = %.4f\n", pnm, (double)*(float *)il.object_unbox(o));
                else if ((ty == 0 || ty == 1) && m_getv && (o = invoke_on(mat, m_getv, na, "GetVector"))) {
                    float v[4]; memcpy(v, il.object_unbox(o), sizeof v);
                    fprintf(stderr, "  [mprobe]     %s = (%.3f %.3f %.3f %.3f)\n", pnm,
                            (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
                } else
                    fprintf(stderr, "  [mprobe]     %s (type %d)\n", pnm, ty);
            }
        }
    }
    fprintf(stderr, "  [mprobe f%u] rails visible: one-hand %d, two-hand %d, left %d, right %d, other lines %d\n",
            frame, one, two, left, right, other);
}


// The system supplies a selection ray only for a deliberate pinch. Consume it
// on Unity's thread, preserving the anatomical hand stream and normal gestures.
#include <pthread.h>
#include <math.h>
static pthread_mutex_t brink_selection_mu=PTHREAD_MUTEX_INITIALIZER;
static struct { float v[6]; int hand,kind; float movement[2]; } brink_selection_queue[8];
static unsigned brink_selection_read,brink_selection_write;
void kl_brink_selection_push(float ox,float oy,float oz,float dx,float dy,float dz,int hand,int kind,float mx,float my) {
    float v[6]={ox,oy,oz,dx,dy,dz};
    for(int i=0;i<6;i++) if(!isfinite(v[i])) return;
    float len=sqrtf(dx*dx+dy*dy+dz*dz); if(len<0.1f||len>10) return;
    pthread_mutex_lock(&brink_selection_mu);
    if(brink_selection_write-brink_selection_read>=8) ++brink_selection_read;
    unsigned at=brink_selection_write++%8;
    memcpy(brink_selection_queue[at].v,v,sizeof v);brink_selection_queue[at].hand=hand;
    brink_selection_queue[at].kind=kind;brink_selection_queue[at].movement[0]=mx;brink_selection_queue[at].movement[1]=my;
    pthread_mutex_unlock(&brink_selection_mu);
}
static int brink_bool(Il2CppObject *o,const MethodInfo *m) {
    Il2CppObject *v=m?invoke_on(o,m,NULL,"look-pinch enabled"):NULL;
    return v && *(unsigned char *)il.object_unbox(v);
}
static int brink_vec(Il2CppObject *o,const MethodInfo *m,float in[3],float out[3]) {
    void *args[]={in};Il2CppObject *v=invoke_on(o,m,args,"look-pinch transform");
    if(!v)return 0;memcpy(out,il.object_unbox(v),12);return 1;
}


static const MethodInfo *brink_selection_children(void) {
    static const MethodInfo *cached;static int attempted;
    if(attempted)return cached;attempted=1;
    kl_image *img=kl_find_image("libil2cpp.so");if(!img)return NULL;
    const MethodInfo *(*next)(Il2CppClass*,void**)=kl_sym(img,"il2cpp_class_get_methods");
    const char *(*name)(const MethodInfo*)=kl_sym(img,"il2cpp_method_get_name");
    uint32_t (*count)(const MethodInfo*)=kl_sym(img,"il2cpp_method_get_param_count");
    void *(*param)(const MethodInfo*,uint32_t)=kl_sym(img,"il2cpp_method_get_param");
    char *(*type_name)(void*)=kl_sym(img,"il2cpp_type_get_name");
    void (*free_name)(void*)=kl_sym(img,"il2cpp_free");
    Il2CppClass *klass=find_class("UnityEngine","GameObject");
    if(!klass||!next||!name||!count||!param||!type_name)return NULL;
    void *it=NULL;const MethodInfo *m;
    while((m=next(klass,&it))) {
        if(strcmp(name(m),"GetComponentsInChildren")||count(m)!=2)continue;
        char *a=type_name(param(m,0)),*b=type_name(param(m,1));
        int match=a&&b&&!strcmp(a,"System.Type")&&!strcmp(b,"System.Boolean");
        if(free_name){if(a)free_name(a);if(b)free_name(b);}
        if(match){cached=m;fprintf(stderr,"  [brink-select] resolved GameObject.GetComponentsInChildren(Type,bool)\n");break;}
    }
    return cached;
}

static Il2CppObject *brink_selection_components(Il2CppObject *owner,Il2CppObject *type) {
    const MethodInfo *go_m=find_method("UnityEngine","Component","get_gameObject",0);
    const MethodInfo *children=brink_selection_children();
    Il2CppObject *go=go_m?invoke_on(owner,go_m,NULL,"button game object"):NULL;
    if(!go||!children)return NULL;
    unsigned char no=0;void *a[]={type,&no};
    return invoke_on(go,children,a,"distance UI components");
}

static int brink_renderer_hit(Il2CppObject *owner,const float ray[6],float *closest) {
    Il2CppClass *rk=find_class("UnityEngine","Renderer");
    const MethodInfo *children=brink_selection_children();
    const MethodInfo *bounds=find_method("UnityEngine","Renderer","get_bounds",0);
    const MethodInfo *enabled=find_method("UnityEngine","Renderer","get_enabled",0);
    if(!rk||!children||!bounds)return 0;
    Il2CppObject *type=il.type_get_object(il.class_get_type(rk));
    Il2CppObject *renderers=brink_selection_components(owner,type);
    unsigned n=renderers?il.array_length(renderers):0;if(n>2048)return 0;
    int found=0;
    for(unsigned i=0;i<n;i++) {
        Il2CppObject *r=((Il2CppObject**)((char*)renderers+ARR_DATA))[i];
        if(!r||(enabled&&!brink_bool(r,enabled)))continue;
        Il2CppObject *boxed=invoke_on(r,bounds,NULL,"distance button bounds");if(!boxed)continue;
        float *b=il.object_unbox(boxed),lo=0,hi=*closest;int valid=1;
        if(b[3]+b[4]+b[5]<1e-5f)continue;
        for(int c=0;c<3;c++) {
            float extent=fmaxf(b[c+3],0.002f),mn=b[c]-extent,mx=b[c]+extent;
            if(fabsf(ray[c+3])<1e-6f){if(ray[c]<mn||ray[c]>mx){valid=0;break;}}
            else {float t0=(mn-ray[c])/ray[c+3],t1=(mx-ray[c])/ray[c+3];if(t0>t1){float t=t0;t0=t1;t1=t;}lo=fmaxf(lo,t0);hi=fminf(hi,t1);if(lo>hi){valid=0;break;}}
        }
        if(valid&&hi>0&&lo<*closest){*closest=lo;found=1;}
    }
    return found;
}

static void brink_look_pinch(void) {
    extern const char *kl_driver_target_name(void);
    const char *t=kl_driver_target_name();if(!t||strcmp(t,"x3635172946605196"))return;
    float input[6],movement[2]; int hand,kind;
    pthread_mutex_lock(&brink_selection_mu);
    if(brink_selection_read==brink_selection_write){pthread_mutex_unlock(&brink_selection_mu);return;}
    unsigned at=brink_selection_read++%8;memcpy(input,brink_selection_queue[at].v,sizeof input);hand=brink_selection_queue[at].hand;kind=brink_selection_queue[at].kind;memcpy(movement,brink_selection_queue[at].movement,sizeof movement);
    pthread_mutex_unlock(&brink_selection_mu);
    if(!mprobe_init())return;
    const MethodInfo *cam_m=find_method("UnityEngine","Camera","get_main",0);
    const MethodInfo *tr_m=find_method("UnityEngine","Component","get_transform",0);
    const MethodInfo *point_m=find_method("UnityEngine","Transform","TransformPoint",1);
    const MethodInfo *dir_m=find_method("UnityEngine","Transform","TransformDirection",1);
    const MethodInfo *active_m=find_method("UnityEngine","Behaviour","get_isActiveAndEnabled",0);
    const MethodInfo *children_m=brink_selection_children();
    const MethodInfo *ray_m=find_method("UnityEngine","Collider","Raycast",3);
    Il2CppClass *bk=find_class("","ButtonBase"), *ck=find_class("UnityEngine","Collider");
    if(!cam_m||!tr_m||!point_m||!dir_m||!active_m||!children_m||!ray_m||!bk||!ck){fprintf(stderr,"  [brink-select] managed selection API unavailable\n");return;}
    Il2CppObject *cam=invoke(cam_m,NULL,"selection camera"),*tr=cam?invoke_on(cam,tr_m,NULL,"camera transform"):NULL;
    if(!tr)return;
    float ray[6];if(!brink_vec(tr,point_m,input,ray)||!brink_vec(tr,dir_m,input+3,ray+3))return;
    float len=sqrtf(ray[3]*ray[3]+ray[4]*ray[4]+ray[5]*ray[5]);if(len<1e-6f)return;
    for(int i=3;i<6;i++)ray[i]/=len;
    uint32_t n=0;Il2CppObject *objects=mprobe_objects(il.type_get_object(il.class_get_type(bk)),&n);
    Il2CppObject *chosen=NULL;float closest=100;
    const MethodInfo *enabled=il.class_get_method_from_name(bk,"get_Enabled",0);
    const MethodInfo *select=il.class_get_method_from_name(bk,"Select",2),*deselect=il.class_get_method_from_name(bk,"Deselect",1);
    if(objects&&n<2048&&select&&deselect) for(uint32_t i=0;i<n;i++) {
        Il2CppObject *button=((Il2CppObject **)((char*)objects+ARR_DATA))[i];
        if(!button||!brink_bool(button,active_m)||!brink_bool(button,enabled))continue;
        if(brink_renderer_hit(button,ray,&closest)) chosen=button;
        Il2CppObject *type=il.type_get_object(il.class_get_type(ck));
        Il2CppObject *cols=brink_selection_components(button,type);if(!cols)continue;
        unsigned nc=il.array_length(cols);if(nc>64)continue;
        for(unsigned j=0;j<nc;j++) {
            Il2CppObject *col=((Il2CppObject **)((char*)cols+ARR_DATA))[j];if(!col)continue;
            // RaycastHit layout: point, normal, faceID, distance, uv, colliderID.
            // Reserve more than its 44-byte native value-type payload.
            float hit[32]={0};void *ra[]={ray,hit,&closest};
            Il2CppObject *ok=invoke_on(col,ray_m,ra,"button raycast");
            if(ok&&*(unsigned char*)il.object_unbox(ok)&&hit[7]>0&&hit[7]<closest){chosen=button;closest=hit[7];}
        }
    }
    if(kind==1) {
        Il2CppClass *sk=find_class("","LocationsMenuScroller"),*ik=find_class("","LocationsMenuItem");
        const MethodInfo *center=sk?il.class_get_method_from_name(sk,"GetCenteredItem",0):NULL;
        const MethodInfo *scroll=sk?il.class_get_method_from_name(sk,"ScrollToItem",1):NULL;
        const MethodInfo *pos=find_method("UnityEngine","Transform","get_position",0);
        const MethodInfo *inverse=find_method("UnityEngine","Transform","InverseTransformPoint",1);
        kl_image *img=kl_find_image("libil2cpp.so");
        void *(*field)(Il2CppClass*,const char*)=img?(void*)kl_sym(img,"il2cpp_class_get_field_from_name"):NULL;
        void (*get)(Il2CppObject*,void*,void*)=img?(void*)kl_sym(img,"il2cpp_field_get_value"):NULL;
        void *container_f=sk&&field?field(sk,"ItemsContainer"):NULL;
        uint32_t ns=0,ni=0;
        Il2CppObject *scrollers=sk?mprobe_objects(il.type_get_object(il.class_get_type(sk)),&ns):NULL;
        Il2CppObject *items=ik?mprobe_objects(il.type_get_object(il.class_get_type(ik)),&ni):NULL;
        if(scrollers&&items&&ns<16&&ni<1024&&center&&scroll&&pos&&inverse&&container_f&&get)
        for(unsigned si=0;si<ns;si++) {
            Il2CppObject *sc=((Il2CppObject**)((char*)scrollers+ARR_DATA))[si];
            if(!brink_bool(sc,active_m))continue;
            Il2CppObject *current=invoke_on(sc,center,NULL,"centered destination"),*container=NULL;
            get(sc,container_f,&container);if(!current||!container)continue;
            // Require this pinch's gaze ray to hit the carousel, not scenery.
            Il2CppObject *ct=il.type_get_object(il.class_get_type(ck));
            Il2CppObject *cols=brink_selection_components(sc,ct);int hit_carousel=0;
            unsigned nc=cols?il.array_length(cols):0;
            if(nc<2048)for(unsigned j=0;j<nc&&!hit_carousel;j++) {
                Il2CppObject *col=((Il2CppObject**)((char*)cols+ARR_DATA))[j];float hit[32]={0},far=100;void *ra[]={ray,hit,&far};
                Il2CppObject *ok=col?invoke_on(col,ray_m,ra,"carousel raycast"):NULL;
                hit_carousel=ok&&*(unsigned char*)il.object_unbox(ok);
            }
            if(!hit_carousel){float far=100;hit_carousel=brink_renderer_hit(sc,ray,&far);}
            if(!hit_carousel)continue;
            Il2CppObject *curtr=invoke_on(current,tr_m,NULL,"destination transform");
            Il2CppObject *cp=curtr?invoke_on(curtr,pos,NULL,"destination position"):NULL;
            float cur[3];if(!cp||!brink_vec(container,inverse,il.object_unbox(cp),cur))continue;
            float direction=fabsf(movement[0])>=fabsf(movement[1])?-movement[0]:movement[1];
            float best=1e9f;Il2CppObject *next=NULL;
            for(unsigned j=0;j<ni;j++) {
                Il2CppObject *item=((Il2CppObject**)((char*)items+ARR_DATA))[j];if(item==current||!brink_bool(item,active_m))continue;
                Il2CppObject *it=invoke_on(item,tr_m,NULL,"destination transform");
                const MethodInfo *child=find_method("UnityEngine","Transform","IsChildOf",1);void *ia[]={container};
                Il2CppObject *is_child=it&&child?invoke_on(it,child,ia,"carousel membership"):NULL;
                if(!is_child||!*(unsigned char*)il.object_unbox(is_child))continue;
                Il2CppObject *ip=invoke_on(it,pos,NULL,"destination position");float local[3];
                if(!ip||!brink_vec(container,inverse,il.object_unbox(ip),local))continue;
                float gap=(local[0]-cur[0])*(direction>0?1:-1);
                if(gap>0.001f&&gap<best){best=gap;next=item;}
            }
            if(next){void *args[]={next};invoke_on(sc,scroll,args,"distance destination swipe");fprintf(stderr,"  [brink-select] distance swipe advanced carousel\n");}
            else fprintf(stderr,"  [brink-select] carousel swipe reached boundary\n");
            return;
        }
        fprintf(stderr,"  [brink-select] swipe gaze missed an active carousel\n");return;
    }
    static unsigned select_trace;
    if(select_trace++<10) fprintf(stderr,"  [brink-select] candidates=%u world ray o=(%.2f %.2f %.2f) d=(%.2f %.2f %.2f)\n",n,ray[0],ray[1],ray[2],ray[3],ray[4],ray[5]);
    if(chosen) {
        unsigned char no=0,yes=1;void *a[]={&no,&yes};invoke_on(chosen,select,a,"look-pinch press");
        void *d[]={&no};invoke_on(chosen,deselect,d,"look-pinch release");
        fprintf(stderr,"  [brink-select] hand%d activated %s at %.3fm\n",hand,il.class_get_name(il.object_get_class(chosen)),closest);
    } else fprintf(stderr,"  [brink-select] hand%d ray did not hit an enabled ButtonBase\n",hand);
}

void kl_mprobe_tick(unsigned frame) {
    brink_look_pinch();
    brink_real_hands(frame);
    brink_wrist_ui(frame);
    shave_input_bridge();
    {
        static int on = -1;
        if (on < 0) on = kl_env_on("KL_PROBE_RAILS", 0);
        if (on && frame >= 800 && frame <= 1400 && frame % 15 == 0 && mprobe_init()) probe_rails(frame);
    }
    {
        static int on = -1; static unsigned every;
        if (on < 0) {
            on = kl_env_on("KL_PROBE_LINES", 0);
            every = (unsigned)kl_env_uint("KL_PROBE_LINES_EVERY", 150);
            if (!every) every = 150;
        }
        if (on && frame >= 600 && frame % every == 0 && mprobe_init()) {
            probe_lines(frame);
            probe_thin_renderers(frame);
            probe_green_renderers(frame);
        }
    }
    // Independent of KL_PROBE_INPUT and of KL_PROBE_FROM: this one has to land
    // BEFORE the throw it exists to describe, and the throw here is on the
    // first frame. It runs once, on the first tick that finds IL2CPP up.
    static int st_done;
    if (!st_done && kl_env_str("KL_PROBE_STACKTRACE", NULL) && mprobe_init()) {
        st_done = 1;
        probe_stack_traces();
    }
    // pc->method naming, once, late enough that the game's assemblies are all
    // loaded (frame 600 ~= ten seconds in).
    static int rs_done;
    if (!rs_done && frame >= 600 && kl_env_str("KL_PROBE_RESOLVE", NULL) && mprobe_init()) {
        rs_done = 1;
        mprobe_resolve_offsets();
    }
    // KL_PROBE_WATCH=<slot>:<offset>[,...] - a static byte of the class behind a
    // metadata slot (see KL_PROBE_CLASSAT; the statics are what compiled code
    // reads through `[class, #0xb8]`), printed with the frame whenever it
    // changes. For a flag a guest waits on: when it flips, or that it never does.
    {
        static const char *spec; static int init;
        enum { MAXW = 16 };
        static uintptr_t slot[MAXW]; static unsigned woff[MAXW]; static int last[MAXW]; static int nw;
        // `<slot>:<off>><off2>`: the static at <off> is an object; watch the byte at
        // <off2> inside it. A trailing `p` watches a pointer for being set at all.
        static int woff2[MAXW]; static int wptr[MAXW];
        if (!init) {
            init = 1;
            spec = kl_env_str("KL_PROBE_WATCH", NULL);
            for (const char *p = spec; p && *p && nw < MAXW; ) {
                char *end = NULL;
                slot[nw] = (uintptr_t)strtoull(p, &end, 16);
                woff[nw] = (end && *end == ':') ? (unsigned)strtoul(end + 1, &end, 0) : 0;
                woff2[nw] = (end && *end == '>') ? (int)strtoul(end + 1, &end, 0) : -1;
                wptr[nw] = end && *end == 'p';
                last[nw] = -2;
                nw++;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        kl_image *img = nw ? kl_find_image("libil2cpp.so") : NULL;
        for (int i = 0; img && i < nw; i++) {
            uintptr_t var = *(uintptr_t *)((uintptr_t)kl_base(img) + slot[i]);
            uintptr_t k = (var >= 0x100000000ull && !(var & 7)) ? *(uintptr_t *)var : 0;
            int now = -1;                                   // class not there yet
            if (k >= 0x100000000ull && !(k & 7)) {
                uintptr_t statics = *(uintptr_t *)(k + 0xb8);
                if (statics >= 0x100000000ull) {
                    uintptr_t at = statics + woff[i];
                    if (woff2[i] >= 0) {
                        uintptr_t obj = *(uintptr_t *)at;
                        at = obj >= 0x100000000ull ? obj + (unsigned)woff2[i] : 0;
                    }
                    if (at) now = wptr[i] ? (*(uintptr_t *)at != 0) : *(unsigned char *)at;
                }
            }
            if (now != last[i]) {
                fprintf(stderr, "  [mprobe] watch %#llx:%u>%d%s = %d at frame %u\n", (unsigned long long)slot[i], woff[i], woff2[i],
                        wptr[i] ? "p" : "", now, frame);
                last[i] = now;
            }
        }
    }
    // Twice, so a slot a later scene initialises is named as well.
    static int ca_done;
    if (ca_done < 2 && frame >= (unsigned)(ca_done ? kl_env_int("KL_PROBE_CLASSAT_AGAIN", 2400) : 600)
        && kl_env_str("KL_PROBE_CLASSAT", NULL) && mprobe_init()) {
        ca_done++;
        mprobe_class_at();
    }
    // KL_PROBE_TIMESCALE=<x>: UnityEngine.Time.timeScale, set again every 120
    // frames from KL_PROBE_CALL_FROM on (a guest sets its own at pauses and
    // scene changes). A guest's own test sequence waits in game seconds; at 10
    // a course that takes it ten minutes is done in one.
    {
        static float ts = -1.0f;
        static unsigned ts_from;
        static const MethodInfo *m_ts;
        if (ts < 0.0f) {
            const char *e = kl_env_str("KL_PROBE_TIMESCALE", NULL);
            ts = e && *e ? (float)atof(e) : 0.0f;
            ts_from = kl_env_uint("KL_PROBE_CALL_FROM", 3000u);
        }
        if (ts > 0.0f && frame >= ts_from && frame % 120 == 0 && mprobe_init()) {
            if (!m_ts) {
                m_ts = find_method("UnityEngine", "Time", "set_timeScale", 1);
                if (m_ts) fprintf(stderr, "  [mprobe f%u] Time.timeScale = %.1f from here on\n", frame, (double)ts);
                else ts = 0.0f;
            }
            if (m_ts) { void *args[1] = { &ts }; invoke(m_ts, args, "Time.set_timeScale"); }
        }
    }
    // KL_PROBE_TIME=1: the guest's clock as Unity keeps it — the physics step
    // (Time.fixedDeltaTime), the frame's delta and the frame rate it asked for
    // — every 300 frames from frame 300. For a held object that shakes while
    // the hand holding it does not: a physics step that is not the frame's.
    {
        static int on = -1;
        static const MethodInfo *m_fdt, *m_dt, *m_mdt, *m_tfr;
        if (on < 0) on = kl_env_on("KL_PROBE_TIME", 0);
        if (on && frame >= 300 && frame % 300 == 0 && mprobe_init()) {
            if (!m_fdt) {
                m_fdt = find_method("UnityEngine", "Time", "get_fixedDeltaTime", 0);
                m_dt  = find_method("UnityEngine", "Time", "get_deltaTime", 0);
                m_mdt = find_method("UnityEngine", "Time", "get_maximumDeltaTime", 0);
                m_tfr = find_method("UnityEngine", "Application", "get_targetFrameRate", 0);
            }
            Il2CppObject *o = m_tfr ? invoke(m_tfr, NULL, "Application.targetFrameRate") : NULL;
            fprintf(stderr, "  [mprobe f%u] time: fixedDeltaTime %.5f s (%.2f Hz), deltaTime %.5f, "
                            "maximumDeltaTime %.4f, targetFrameRate %d\n", frame,
                    m_fdt ? (double)invoke_float(m_fdt, NULL, "Time.fixedDeltaTime") : -1.0,
                    m_fdt ? 1.0 / (double)invoke_float(m_fdt, NULL, "Time.fixedDeltaTime") : -1.0,
                    m_dt ? (double)invoke_float(m_dt, NULL, "Time.deltaTime") : -1.0,
                    m_mdt ? (double)invoke_float(m_mdt, NULL, "Time.maximumDeltaTime") : -1.0,
                    o ? *(int32_t *)il.object_unbox(o) : -999);
        }
    }
    static int call_done;
    static unsigned call_from;
    if (!call_done && kl_env_str("KL_PROBE_CALL", NULL)) {
        if (!call_from) call_from = kl_env_uint("KL_PROBE_CALL_FROM", 3000u);
        if (frame >= call_from && mprobe_init()) { call_done = 1; mprobe_keep(frame); mprobe_call(frame); }
    }
    // KL_PROBE_KEEP without KL_PROBE_CALL: only the listing (and the filter), at
    // the same frame — how a walk learns the names a guest loads its scenes by.
    static int keep_done;
    if (!keep_done && !kl_env_str("KL_PROBE_CALL", NULL) && kl_env_str("KL_PROBE_KEEP", NULL)
        && frame >= kl_env_uint("KL_PROBE_CALL_FROM", 3000u) && mprobe_init()) {
        keep_done = 1;
        mprobe_keep(frame);
    }
    if (kl_env_str("KL_PROBE_SCRIPT", NULL)) mprobe_script(frame);
    if (frame % 300 == 0 && kl_env_str("KL_PROBE_STRINGS", NULL) && mprobe_init()) mprobe_strings(frame);
    static int find_done;
    if (!find_done && frame >= 600 && kl_env_str("KL_PROBE_FIND", NULL) && mprobe_init()) {
        find_done = 1;
        mprobe_find_methods();
    }
    // Also independent, and for the same reason: the disagreement it looks for
    // is settled long before KL_PROBE_FROM's default, and reading XRSettings
    // runs no cctor the guest has not already run.
    static unsigned xr_from, xr_every;
    if (kl_env_str("KL_PROBE_XR", NULL)) {
        if (!xr_every) {
            xr_from  = kl_env_uint("KL_PROBE_XR_FROM", 60u);
            xr_every = kl_env_uint("KL_PROBE_XR_EVERY", 300u);
            if (!xr_every) xr_every = 300;
        }
        if (frame >= xr_from && frame % xr_every == 0 && mprobe_init())
            probe_xr_display(frame);
    }

    static int on = -1;
    if (on < 0) on = kl_env_on("KL_PROBE_INPUT", 0);
    // The gate this file's header and its caller both describe, and which was
    // not here: `on` was read and never looked at, so every IL2CPP guest was
    // probed every 120 frames from frame 1200 on — on a headset, in every run.
    // Each probe calls into the game's own managed code on the frame thread
    // (OVRInput, the event system, FindObjectsOfType for the menu objects)
    // and prints four lines; Walkabout's log shows it every 1.2 s for a whole
    // session, beside a frame that arrives a refresh late about as often
    // (2026-10-11). Off unless asked for, as documented.
    if (!on) return;
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
    if (!dumped_enums) { dumped_enums = 1; probe_dump_enums(); probe_dump_methods(); }
    probe_xform_path(frame);
    probe_fields(frame);

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
    //    Gated behind its OWN knob (default off) even under KL_PROBE_INPUT: on a
    //    title whose Input Manager has no axis of the queried name, il2cpp's
    //    Input.GetAxis walks off a buffer inside libunity and SIGSEGVs — an
    //    intermittent native fault the managed try/catch cannot stop, and it
    //    killed some Unity guests mid-run. The answer here is always
    //    "InvalidOperationException" anyway, so it is off unless asked for.
    if (kl_env_on("KL_PROBE_GETAXIS", 0)) {
        fprintf(stderr, "  [mprobe f%u] axes:", frame);
        for (unsigned i = 0; i < N_AXES; i++) {
            void *args[1] = { axis_str[i] };
            fprintf(stderr, " %s=%.3f", AXES[i], invoke_float(m_axis, args, "Input.GetAxis"));
        }
        fprintf(stderr, "\n");
    }

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
