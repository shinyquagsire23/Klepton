// Internal seam between kl_jni.c (the JNIEnv/JavaVM mechanism) and the
// per-family Java class implementations beside it. Not a public header: nothing
// outside runtime/jni/ includes it, and kl_jni.h remains the runtime's only
// JNI-facing interface.
//
// The mechanism owns object/class/id registries and dispatch; each
// kl_jni_<family>.c owns one guest family's Java classes and exports exactly two
// symbols back — its binding table and, where it has one, its field table. The
// tables are walked as a NULL-terminated list of tables (klj_binding_tables),
// so a family's implementation functions stay static to its own file.
#ifndef KL_JNI_INT_H
#define KL_JNI_INT_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "kl_jni.h"
#include "kl_va.h"

#define KLJ_LOG(...) do { fprintf(stderr, "[jni] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define KLJ_MAX_CLASSES 512
#define KLJ_MAX_NATIVES 1024
#define KLJ_MAX_WANTED  1024
#define KLJ_OBJ_MAGIC 0x4B4C4A4FU   /* 'KLJO' */
#define KLJ_MAX_OBJECTS (8192*16)   // parenthesised: a bare 8192*16 turns
                                    // "x % KLJ_MAX_OBJECTS" into (x%8192)*16
#define KLJ_MAX_ARGS 16

// Every jobject handed to the guest is one of these. The guest can only obtain
// an object from us, so the magic makes GetObjectClass answerable and catches a
// jobject arriving from anywhere else instead of dereferencing it blindly.
typedef struct klj_object {
    uint32_t    magic;
    uint32_t    pinned; // global-ref count, or 1 forever for interned class
                        // objects and host singletons: DeleteLocalRef and
                        // PopLocalFrame must not recycle it while > 0
    const char *cls;    // interned class name
    void       *data;   // java/lang/String -> char*; java/lang/Class -> klj_class*
    // Recorded by whoever allocated the payload rather than switched on `cls` at
    // recycle time: payloads are genuinely shared in places (a Looper object and
    // its thread's Looper are one klj_looper), and a class-keyed destructor would
    // double-free exactly those. An object with no destructor leaks its payload.
    void      (*destroy)(void *data);
} klj_object;

typedef struct { char name[224]; klj_object *as_object; } klj_class;

typedef struct { const char *cls, *name, *sig; void *fn; } klj_native;

// Every method/field id the guest asked for. The address of an entry IS the
// jmethodID/jfieldID handed back, so identity is (class, name, signature) —
// exactly what JNI guarantees.
typedef struct {
    const char *cls;
    char        name[160], sig[224];
    char        kind;   // m=method M=static method f=field F=static field
} klj_wanted;

// The two object kinds the host constructs rather than merely hands back: a
// bitter/jnibridge proxy, and the java.lang.reflect.Method describing what to
// invoke on it.
#define KLJ_CLASS_PROXY  "bitter/jnibridge/JNIBridge$Proxy"
#define KLJ_CLASS_METHOD "java/lang/reflect/Method"
#define KLJ_CLASS_FIELD  "java/lang/reflect/Field"
typedef struct { const char *cls, *name, *sig; int is_static; } klj_method_obj;
typedef struct { const char *cls, *name, *sig; int is_static; } klj_field_obj;

// A boxed primitive or a SharedPreferences value: a preference is exactly "a
// typed scalar with a key", so one shape serves both.
typedef struct {
    char   *key;
    char    kind;     // 'S' string, 'I' 'J' 'Z' integral, 'F' float, '-' pending removal
    char   *sval;
    int64_t ival;
    float   fval;
} klj_pref;

// A Bundle's payload: a NULL-terminated key/value table.
typedef struct { const char *key, *val; } klj_kv;

// One representation for both array flavours: object arrays hold jobjects,
// primitive arrays hold a raw buffer. `kind` is the JNI type character, so the
// class name is "[" + kind (or "[L<elem>;") — what GetObjectClass must answer.
typedef struct {
    int    len;
    char   kind;    // 'L' object, else 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D'
    size_t elem;    // element size in bytes (primitive arrays)
    void  *data;    // void** when kind=='L', else a raw buffer
} klj_array;

typedef struct { void *addr; int64_t capacity; } klj_direct_buffer;

typedef union { uint64_t j; double d; void *l; } klj_val;
typedef klj_val (*klj_impl)(void *env, void *self, const klj_val *argv, int argc);
typedef struct { const char *cls, *name, *sig; klj_impl fn; } klj_binding;

// The `A` calling convention: arguments arrive as an array of jvalue unions.
// Each jvalue is 8 bytes but a narrow type writes only its own width, so the
// member must be read at the declared type — reading .j for a jint picks up
// whatever four bytes the caller left above it.
typedef union {
    uint8_t  z;  int8_t  b;  uint16_t c;  int16_t s;
    kl_jint  i;  int64_t j;  float    f;  double  d;  void *l;
} klj_jvalue;

typedef klj_val (*klj_field_fn)(void);   // for values only known at runtime

typedef struct {
    const char  *cls, *name, *sig;
    const char  *sval;    // for object fields — String constants
    int64_t      ival;    // for integral primitive fields
    double       dval;    // for float/double fields — a separate slot, because
                          // klj_val is a union and the caller's `want` decides
                          // which half of it is read
    klj_field_fn fn;      // takes precedence: paths and other configured values
} klj_field;

// Interned once in kl_jni.c: klj_object holds a bare `const char *cls`, so these
// must be one address across every translation unit.
extern const char KLJ_CLASS_CLASS[];
extern const char KLJ_CLASS_STRING[];

extern pthread_mutex_t g_lock;

// ---------------------------------------------------------------- mechanism
// Provided by kl_jni.c to the family implementations.
klj_object *klj_as_object(void *p);
klj_object *klj_alloc_object_locked(const char *cls, void *data);
void        klj_retire_object_locked(klj_object *o);
void        klj_frame_forget(klj_object *o);
klj_class  *klj_intern_class_locked(const char *name);
const char *klj_class_name(void *clazz);
void       *klj_class_object(const char *class_name);
void       *klj_new_object_data(const char *class_name, void *data);
void       *klj_own(void *obj, void (*destroy)(void *));
const char *klj_str(void *s);
klj_array  *klj_arr(void *a);
void       *klj_new_array(char kind, const char *elem_cls, int len);
klj_direct_buffer *klj_direct(void *buf);
void       *klj_want(void *clazz, const char *name, const char *sig, char kind);
klj_val     klj_call(void *env, void *self, void *mid, kl_va *va, char want);
void        klj_field_store(void *obj, void *fid, klj_val v);
kl_jint     klj_ExceptionCheck(void *env);
void       *klj_GetObjectClass(void *env, void *obj);
kl_jint     klj_PushLocalFrame(void *env, kl_jint cap);
void       *klj_PopLocalFrame(void *env, void *result);
void       *klj_NewObjectV(void *env, void *clazz, void *mid, kl_va *va);
void       *klj_NewDirectByteBuffer(void *env, void *address, int64_t capacity);
kl_jint     klj_RegisterNatives(void *env, void *clazz, const kl_jni_method *m, kl_jint n);

// ---------------------------------------------------------------- field seam
// Runtime-valued fields: defined by the family that owns the underlying answer,
// referenced from g_fields in kl_jni.c.
void       *klj_new_file(const char *path);
const char *klj_guest_package(void);
klj_val     klj_PackageInfo_versionCode(void);
klj_val     klj_PackageInfo_versionName(void);
klj_val     klj_PackageInfo_packageName(void);
klj_val     klj_appinfo_sourceDir(void);
klj_val     klj_appinfo_nativeLibraryDir(void);
klj_val     klj_appinfo_dataDir(void);
klj_val     klj_appinfo_splitSourceDirs(void);
klj_val     klj_Uri_EMPTY(void);
klj_val     klj_metaData_field(void);
klj_val     klj_currentActivity_field(void);
klj_val     klj_porterduff_clear(void);

// ---------------------------------------------------------------- the tables
// One per family file, plus the NULL-terminated lists kl_jni.c resolves through.
extern const klj_binding klj_bind_lang[];
extern const klj_binding klj_bind_android[];
extern const klj_binding klj_bind_looper[];
extern const klj_binding klj_bind_display[];
extern const klj_binding klj_bind_window[];
extern const klj_binding klj_bind_net[];
extern const klj_binding klj_bind_bridge[];
extern const klj_binding klj_bind_softinput[];
extern const klj_binding klj_bind_prefs[];
extern const klj_binding klj_bind_io[];
extern const klj_binding klj_bind_sdl[];
extern const klj_binding klj_bind_ue4[];
extern const klj_binding klj_bind_jkxr[];
extern const klj_binding klj_bind_services[];
extern const klj_binding *const klj_binding_tables[];



// The device description groups. Answered as one description rather than call
// by call: the guest cross-checks members against each other, so two answers
// that disagree are worse than either alone.
// ---- the presented display ----
// One description of the screen, read by everything that asks about it: the
// Display, the DisplayMetrics fields, and Resources. Deciding it in one place is
// the point — Unity derives its render target size, its UI scale and its frame
// pacing from these numbers, and answering each call on its own terms would let
// them disagree with each other and with the ANativeWindow in kl_ndk.c.
//
// The geometry is a Quest 2's per-eye panel, matching that window, and for the
// same reason Build.MODEL is a Quest 2's below: this title branches on Oculus
// hardware. It is a placeholder in the same sense kl_ndk.c's is — in VR the eye
// buffers come from the XR runtime rather than from the Android display,
// and this exists to give Unity a coherent non-zero screen at startup. 72 Hz is
// the Quest 2's default mode; 90 is opt-in, and claiming it would have Unity
// pace to a rate the GL path cannot yet deliver. Density is a choice rather than a
// measurement — it only scales 2D UI — and xhdpi suits a panel this fine.
//
// Macros rather than a struct because g_fields is a static initialiser and a
// const object is not a constant expression in C.
#define KLJ_DISPLAY_W        1832
#define KLJ_DISPLAY_H        1920
#define KLJ_DISPLAY_DPI      320
#define KLJ_DISPLAY_DENSITY  2.0     /* xhdpi: densityDpi / 160 */
#define KLJ_DISPLAY_XDPI     320.0
#define KLJ_DISPLAY_YDPI     320.0
#define KLJ_DISPLAY_REFRESH  72.0f
#define KLJ_DISPLAY_ROTATION 0       /* Surface.ROTATION_0 */

// NetworkCapabilities.TRANSPORT_* / NET_CAPABILITY_*, from android-34's
// android.jar. Defined here rather than beside their handlers because both
// g_fields (which hands the guest the numbers) and klj_NetworkCapabilities_*
// (which answers questions phrased in them) have to mean the same thing by
// them — the one bug this whole family is prone to is the two drifting apart.
#define KLJ_NC_TRANSPORT_CELLULAR   0
#define KLJ_NC_TRANSPORT_WIFI       1
#define KLJ_NC_TRANSPORT_BLUETOOTH  2
#define KLJ_NC_TRANSPORT_ETHERNET   3
#define KLJ_NC_TRANSPORT_VPN        4
#define KLJ_NC_CAP_NOT_METERED     11
#define KLJ_NC_CAP_INTERNET        12
#define KLJ_NC_CAP_NOT_RESTRICTED  13
#define KLJ_NC_CAP_TRUSTED         14
#define KLJ_NC_CAP_NOT_VPN         15
#define KLJ_NC_CAP_VALIDATED       16
#define KLJ_NC_CAP_NOT_ROAMING     18
#define KLJ_NC_CAP_NOT_CONGESTED   20
#define KLJ_NC_CAP_NOT_SUSPENDED   21

// ---------------------------------------------------------------- audio
//
// Answered as one description of an audio device rather than call by call, for
// the same reason as the display group: Unity's FMOD asks for the sample rate
// and the buffer size separately and then sizes its mixer from both, so two
// answers that disagree are worse than either alone. 48 kHz / 256 frames is the
// low-latency configuration a Quest 2 reports, and it is what the engine had
// already inferred from the driver before it got this far.
#define KLJ_AUDIO_RATE   48000
#define KLJ_AUDIO_FRAMES 256

// ------------------------------------------------- cross-family seams
// Types and definitions one family file owns and another reaches for. The
// original file was ordered by the batch of work that forced each class, so
// these crossings are inherited, not designed.

typedef struct { char *url; char *path; int found, logged; } klj_webdoc;
typedef struct { void **items; int count; } klj_list;
typedef struct { int64_t native_ptr; void *classes; int disabled; } klj_proxy;
typedef struct { void *looper, *callback; } klj_handler;
typedef struct { int32_t what, arg1, arg2; void *obj, *target; } klj_message;
typedef struct {
    char  *data;        // the whole contents, when the stream was slurped...
    FILE  *f;           // ...or the file it is a window into (then data == NULL)
    size_t base;        // where that window starts in that file
    size_t len, pos;
} klj_stream;
#define KLJ_MAX_UI_TASKS 64
typedef struct {
    void   *runnable;      // Runnable, or NULL when this entry is a message
    void   *message;       // android/os/Message, or NULL when it is a runnable
    int64_t delay_ms;
} klj_ui_task;

// The guest's own paths, set once by the host before any guest code runs.
extern const char *g_apk_path, *g_assets_dir, *g_native_lib_dir;
const char *klj_abspath(const char *p);
void        klj_mkdir_p(const char *path);

// The UI queue: kl_jni_looper.c owns it, kl_jni_bridge.c posts to it.
extern pthread_t    g_ui_thread;
extern int          g_ui_thread_known;
extern void        *g_frame_callback;
extern klj_ui_task  g_ui_tasks[KLJ_MAX_UI_TASKS];
extern unsigned     g_ui_task_n;

klj_val      klj_singleton(const char *cls, void **slot);
void        *klj_new_list(void **items, int count);
klj_webdoc  *klj_webdoc_of(void *self);
klj_handler *klj_as_handler(void *obj);
klj_message *klj_as_message(void *obj);
size_t       klj_stream_read(klj_stream *st, void *buf, size_t want);
void         klj_stream_close(klj_stream *st);
void         klj_stream_free(void *p);
void        *klj_proxy_invoke(void *proxy, const char *iface,
                              const char *name, const char *sig, void *args);
void        *klj_FromReflectedField(void *env, void *field);
int          klj_permission_state(const char *p, const char **why);

// Bindings one family answers on behalf of another family's class.
klj_val klj_List_size(void *env, void *self, const klj_val *a, int n);
klj_val klj_PM_queryIntentActivities(void *env, void *self, const klj_val *a, int n);
klj_val klj_HandlerThread_start(void *env, void *self, const klj_val *a, int n);
klj_val klj_AudioManager_getStreamVolume(void *env, void *self, const klj_val *a, int n);
klj_val klj_Display_getAppVsyncOffsetNanos(void *env, void *self, const klj_val *a, int n);


// ...and the same crossings the compiler found once the families were apart.
const char *klj_activity_class(void);
const char *klj_xml_attr(const char *el, const char *end, const char *attr);
klj_val klj_Display_getPresentationDeadlineNanos(void *env, void *self, const klj_val *a, int n);
klj_val klj_Display_getRefreshRate(void *env, void *self, const klj_val *a, int n);
klj_val klj_UnityPlayer_loadLibrary(void *env, void *self, const klj_val *a, int n);
unsigned klj_drain_soft_input(void);
void *klj_box_int(int32_t v);
void klj_deliver_message(void *message);
void klj_guest_version(long *code, const char **name);

#endif /* KL_JNI_INT_H */
