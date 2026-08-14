// libklepton_openxr — the synthetic OpenXR runtime. See kl_openxr.h for why
// the Khronos loader is replaced rather than translated.
//
// The shape is kl_ovrp.c's, deliberately, because the problem is the same one:
// a large XR API surface where the guest calls an unknown subset, and guessing
// at the subset is how a session gets spent implementing functions nothing ever
// invokes. So every entry point exists and is resolvable, and the ones we have
// not built refuse BY NAME. The list below is the measured import list of
// libvrlink_scene.so, all forty-six of them, in the order the header explains,
// plus the two that arrive only through xrGetInstanceProcAddr.
//
// The file reads in the order the guest walks it, which is also the order it
// was written in — one abort at a time, running the guest between each:
//
//   the surface and the refusal          every name, and how an unbuilt one says so
//   xrGetInstanceProcAddr                the bootstrap, and the extension door
//   the boot sequence                    extensions, instance, system, views
//   the session                          and the state machine xrPollEvent drives
//   spaces                               reference and action frames
//   actions                              bookkeeping, and the binding census
//   swapchains                           the eye images — where this meets P5
//   the frame loop                       wait/begin/end, locate views and spaces
//
// Two things about the ABI, since neither is obvious from the spec text:
//
//   - Every xr* function returns XrResult, an int32 where **0 is XR_SUCCESS**
//     and negatives are failures. Trap 10 is the standing warning here: on the
//     OVRPlugin side, answering a plain "1 for true" to a function that returns
//     a result code produced a *failure* the engine ignored and managed code
//     tripped over three layers away. The same mistake is available here and
//     would look exactly as innocent.
//   - Structures are versioned by a `type` enum in their first field and
//     chained through `next`. We never invent a layout: anything we fill in is
//     transcribed from the specification, and anything we have not transcribed
//     is a refusal rather than a partly-populated struct.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "kl_openxr.h"
#include "kl_ovrp.h"
#include "kl_egl.h"
#include "kl_glfb.h"
#include "kl_env.h"
#include "klepton.h"

// ------------------------------------------------------- transcribed from the spec
// Only what we actually read or write. Every layout below is copied field for
// field from the Khronos openxr.h (1.0.x, and these are all core-since-1.0 so
// the minor version does not move them) — the header is NOT vendored, because
// what we need is a dozen structs and the alternative is 5,000 lines of enum
// that would then have to be kept current for no benefit.
//
// The three rules that keep this honest, all of them learned elsewhere:
//   - Handles are XR_DEFINE_HANDLE, i.e. pointers on LP64. Ours point at our
//     own singletons and carry a magic, so a handle from the wrong door is a
//     named refusal rather than a wild read (trap 3's cousin).
//   - `type`/`next` lead every struct. We never *write* into a `next` we did
//     not recognise, and we log the chain, because a chained struct the guest
//     appended is a capability question it is asking us and silence is the
//     honest answer for one we do not implement.
//   - uint32_t before pointers means padding. Every struct here is written out
//     in full rather than with the fields we care about, so the offsets are the
//     compiler's problem and not ours.
typedef uint64_t XrVersion;
typedef int32_t  XrResult;
typedef uint32_t XrBool32;
typedef uint64_t XrSystemId;

#define XR_MAX_EXTENSION_NAME_SIZE   128
#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE      128
#define XR_MAX_RUNTIME_NAME_SIZE     128
#define XR_MAX_SYSTEM_NAME_SIZE      256

#define XR_MAKE_VERSION(maj, min, pat) \
    ((((uint64_t)(maj) & 0xffffULL) << 48) | (((uint64_t)(min) & 0xffffULL) << 32) | \
     ((uint64_t)(pat) & 0xffffffffULL))

enum {
    XR_TYPE_EXTENSION_PROPERTIES        = 2,
    XR_TYPE_INSTANCE_CREATE_INFO        = 3,
    XR_TYPE_SYSTEM_GET_INFO             = 4,
    XR_TYPE_SYSTEM_PROPERTIES           = 5,
    XR_TYPE_INSTANCE_PROPERTIES         = 32,
    XR_TYPE_VIEW_CONFIGURATION_VIEW     = 41,
    XR_TYPE_SESSION_CREATE_INFO         = 8,
    XR_TYPE_SESSION_BEGIN_INFO          = 10,
    XR_TYPE_EVENT_DATA_BUFFER           = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED = 52,
    XR_TYPE_HAPTIC_VIBRATION            = 13,
    XR_TYPE_HAPTIC_ACTION_INFO          = 59,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_SPACE_LOCATION              = 42,
    XR_TYPE_ACTION_STATE_BOOLEAN        = 23,
    XR_TYPE_ACTION_STATE_FLOAT          = 24,
    XR_TYPE_ACTION_STATE_POSE           = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO      = 28,
    XR_TYPE_ACTION_CREATE_INFO          = 29,
    XR_TYPE_ACTION_SPACE_CREATE_INFO    = 38,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_INTERACTION_PROFILE_STATE   = 53,
    XR_TYPE_ACTION_STATE_GET_INFO       = 58,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO           = 61,
    XR_TYPE_SWAPCHAIN_CREATE_INFO       = 9,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO   = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR = 1000024002,
    XR_TYPE_VIEW_LOCATE_INFO            = 6,
    XR_TYPE_VIEW                        = 7,
    XR_TYPE_VIEW_STATE                  = 11,
    XR_TYPE_FRAME_END_INFO              = 12,
    XR_TYPE_FRAME_WAIT_INFO             = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35,
    XR_TYPE_COMPOSITION_LAYER_QUAD      = 36,
    XR_TYPE_FRAME_STATE                 = 44,
    XR_TYPE_FRAME_BEGIN_INFO            = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR = 1000024001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR = 1000024003,
};

// The session states, and the order is the state machine: a runtime walks an
// app forward through them and the app is only allowed to call xrBeginSession
// in READY and xrEndSession in STOPPING.
enum {
    KLXR_SESSION_STATE_UNKNOWN = 0, KLXR_SESSION_STATE_IDLE = 1,
    KLXR_SESSION_STATE_READY = 2,   KLXR_SESSION_STATE_SYNCHRONIZED = 3,
    KLXR_SESSION_STATE_VISIBLE = 4, KLXR_SESSION_STATE_FOCUSED = 5,
    KLXR_SESSION_STATE_STOPPING = 6, KLXR_SESSION_STATE_LOSS_PENDING = 7,
    KLXR_SESSION_STATE_EXITING = 8,
};

typedef struct { int32_t type; void *next;
                 char extensionName[XR_MAX_EXTENSION_NAME_SIZE];
                 uint32_t extensionVersion; } XrExtensionProperties;

typedef struct { char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
                 uint32_t applicationVersion;
                 char engineName[XR_MAX_ENGINE_NAME_SIZE];
                 uint32_t engineVersion;
                 XrVersion apiVersion; } XrApplicationInfo;

typedef struct { int32_t type; const void *next;
                 uint64_t createFlags;
                 XrApplicationInfo applicationInfo;
                 uint32_t enabledApiLayerCount;
                 const char *const *enabledApiLayerNames;
                 uint32_t enabledExtensionCount;
                 const char *const *enabledExtensionNames; } XrInstanceCreateInfo;

typedef struct { int32_t type; void *next;
                 XrVersion runtimeVersion;
                 char runtimeName[XR_MAX_RUNTIME_NAME_SIZE]; } XrInstanceProperties;

typedef struct { int32_t type; const void *next;
                 int32_t formFactor; } XrSystemGetInfo;

typedef struct { uint32_t maxSwapchainImageHeight, maxSwapchainImageWidth,
                          maxLayerCount; } XrSystemGraphicsProperties;
typedef struct { XrBool32 orientationTracking, positionTracking;
               } XrSystemTrackingProperties;

typedef struct { int32_t type; void *next;
                 XrSystemId systemId;
                 uint32_t vendorId;
                 char systemName[XR_MAX_SYSTEM_NAME_SIZE];
                 XrSystemGraphicsProperties graphicsProperties;
                 XrSystemTrackingProperties trackingProperties; } XrSystemProperties;

typedef struct { int32_t type; void *next;
                 XrVersion minApiVersionSupported,
                           maxApiVersionSupported; } XrGraphicsRequirementsOpenGLESKHR;

typedef struct { int32_t type; const void *next;
                 uint64_t createFlags;
                 XrSystemId systemId; } XrSessionCreateInfo;

typedef struct { int32_t type; const void *next;
                 void *display, *config, *context;
               } XrGraphicsBindingOpenGLESAndroidKHR;

typedef struct { int32_t type; const void *next;
                 int32_t primaryViewConfigurationType; } XrSessionBeginInfo;

typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct { float width, height; } XrExtent2Df;

typedef struct { int32_t type; const void *next;
                 int32_t referenceSpaceType;
                 XrPosef poseInReferenceSpace; } XrReferenceSpaceCreateInfo;

typedef struct { int32_t type; void *next;
                 uint64_t locationFlags;
                 XrPosef pose; } XrSpaceLocation;

// ---- actions. XrPath is an interned string, uint64, 0 = XR_NULL_PATH ----
typedef uint64_t XrPath;

#define XR_MAX_PATH_LENGTH                    256
#define XR_MAX_ACTION_SET_NAME_SIZE           64
#define XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE 128
#define XR_MAX_ACTION_NAME_SIZE               64
#define XR_MAX_LOCALIZED_ACTION_NAME_SIZE     128

typedef struct { int32_t type; const void *next;
                 char actionSetName[XR_MAX_ACTION_SET_NAME_SIZE];
                 char localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
                 uint32_t priority; } XrActionSetCreateInfo;

typedef struct { int32_t type; const void *next;
                 char actionName[XR_MAX_ACTION_NAME_SIZE];
                 int32_t actionType;
                 uint32_t countSubactionPaths;
                 const XrPath *subactionPaths;
                 char localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
               } XrActionCreateInfo;

typedef struct { int32_t type; const void *next;
                 void *action; XrPath subactionPath;
                 XrPosef poseInActionSpace; } XrActionSpaceCreateInfo;

typedef struct { void *action; XrPath binding; } XrActionSuggestedBinding;
typedef struct { int32_t type; const void *next;
                 XrPath interactionProfile;
                 uint32_t countSuggestedBindings;
                 const XrActionSuggestedBinding *suggestedBindings;
               } XrInteractionProfileSuggestedBinding;

typedef struct { int32_t type; const void *next;
                 uint32_t countActionSets;
                 void *const *actionSets; } XrSessionActionSetsAttachInfo;

typedef struct { int32_t type; void *next;
                 XrPath interactionProfile; } XrInteractionProfileState;

typedef struct { void *actionSet; XrPath subactionPath; } XrActiveActionSet;
typedef struct { int32_t type; const void *next;
                 uint32_t countActiveActionSets;
                 const XrActiveActionSet *activeActionSets; } XrActionsSyncInfo;

typedef struct { int32_t type; const void *next;
                 void *action; XrPath subactionPath; } XrActionStateGetInfo;

typedef struct { int32_t type; void *next;
                 XrBool32 currentState, changedSinceLastSync;
                 int64_t lastChangeTime; XrBool32 isActive; } XrActionStateBoolean;
typedef struct { int32_t type; void *next;
                 float currentState; XrBool32 changedSinceLastSync;
                 int64_t lastChangeTime; XrBool32 isActive; } XrActionStateFloat;
typedef struct { int32_t type; void *next;
                 XrBool32 isActive; } XrActionStatePose;

// ---- haptics: the one action family that runs OUT of the guest ----
//
// The feedback argument is polymorphic the same way a composition layer is:
// the app passes a base header and the runtime reads `type` before casting.
// XrHapticVibration is the only type in core OpenXR, but reading the header
// first is what makes an unrecognised one a named refusal rather than four
// bytes of somebody else's struct read as an amplitude.
typedef struct { int32_t type; const void *next;
                 void *action; XrPath subactionPath; } XrHapticActionInfo;
typedef struct { int32_t type; const void *next; } XrHapticBaseHeader;
typedef struct { int32_t type; const void *next;
                 int64_t duration;          // XrDuration, NANOSECONDS
                 float frequency, amplitude; } XrHapticVibration;

// XR_MIN_HAPTIC_DURATION is -1, not 0, and means "the shortest pulse the
// hardware can produce" — a click. A guest asking for it wants an event, not
// silence, so it must not be clamped to zero on the way through.
#define XR_MIN_HAPTIC_DURATION   (-1)
#define XR_FREQUENCY_UNSPECIFIED 0.0f

typedef struct { int32_t type; const void *next;
                 uint64_t createFlags, usageFlags;
                 int64_t  format;
                 uint32_t sampleCount, width, height,
                          faceCount, arraySize, mipCount; } XrSwapchainCreateInfo;

// The image struct is polymorphic by graphics API: the app passes an array of
// whatever its binding's image type is, and the runtime writes into it after
// checking `type`. For GLES that is one uint32 GL texture name per image.
typedef struct { int32_t type; void *next; uint32_t image;
               } XrSwapchainImageOpenGLESKHR;

typedef struct { int32_t type; const void *next; } XrSwapchainImageAcquireInfo;
typedef struct { int32_t type; const void *next;
                 int64_t timeout; } XrSwapchainImageWaitInfo;
typedef struct { int32_t type; const void *next; } XrSwapchainImageReleaseInfo;

// ---- the frame loop ----
typedef struct { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;

typedef struct { int32_t type; const void *next; } XrFrameWaitInfo;
typedef struct { int32_t type; void *next;
                 int64_t predictedDisplayTime, predictedDisplayPeriod;
                 XrBool32 shouldRender; } XrFrameState;
typedef struct { int32_t type; const void *next; } XrFrameBeginInfo;

typedef struct { int32_t x, y; } XrOffset2Di;
typedef struct { int32_t width, height; } XrExtent2Di;
typedef struct { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;
typedef struct { void *swapchain; XrRect2Di imageRect;
                 uint32_t imageArrayIndex; } XrSwapchainSubImage;

// Only the header is common to every layer type. A runtime reads `type` and
// casts, exactly as it does for the swapchain images — so a layer type we do
// not recognise must be SKIPPED, not cast to the one we do.
typedef struct { int32_t type; const void *next;
                 uint64_t layerFlags; void *space; } XrCompositionLayerBaseHeader;

typedef struct { int32_t type; const void *next;
                 XrPosef pose; XrFovf fov;
                 XrSwapchainSubImage subImage; } XrCompositionLayerProjectionView;
typedef struct { int32_t type; const void *next;
                 uint64_t layerFlags; void *space;
                 uint32_t viewCount;
                 const XrCompositionLayerProjectionView *views;
               } XrCompositionLayerProjection;

typedef struct { int32_t type; const void *next;
                 int64_t displayTime;
                 int32_t environmentBlendMode;
                 uint32_t layerCount;
                 const XrCompositionLayerBaseHeader *const *layers;
               } XrFrameEndInfo;

typedef struct { int32_t type; const void *next;
                 int32_t viewConfigurationType;
                 int64_t displayTime;
                 void *space; } XrViewLocateInfo;
typedef struct { int32_t type; void *next;
                 uint64_t viewStateFlags; } XrViewState;
typedef struct { int32_t type; void *next;
                 XrPosef pose; XrFovf fov; } XrView;

// XrEventDataBuffer is the app's inbox: 4,000 bytes it hands us that we write
// the *largest* event structure into. Every event type starts type/next, so the
// app reads `type` and casts — which means writing more than we should into one
// is a buffer the app then reads as a different shape.
typedef struct { int32_t type; const void *next;
                 uint8_t varying[4000]; } XrEventDataBuffer;
typedef struct { int32_t type; const void *next;
                 void *session; int32_t state; int64_t time;
               } XrEventDataSessionStateChanged;
// Carries no profile of its own: the app re-reads
// xrGetCurrentInteractionProfile per top-level path when it sees this.
typedef struct { int32_t type; const void *next;
                 void *session; } XrEventDataInteractionProfileChanged;

typedef struct { int32_t type; void *next;
                 uint32_t recommendedImageRectWidth, maxImageRectWidth;
                 uint32_t recommendedImageRectHeight, maxImageRectHeight;
                 uint32_t recommendedSwapchainSampleCount,
                          maxSwapchainSampleCount; } XrViewConfigurationView;

// The result codes we return. Named as the spec names them so a value in a
// guest log ("error: -7") can be read straight off this list.
enum {
    // successes first (OpenXR has several non-zero ones, and every one of them
    // is >= 0 — a caller testing `== XR_SUCCESS` misreads them all)
    KLXR_SUCCESS                        = 0,
    KLXR_EVENT_UNAVAILABLE              = 4,
    KLXR_SPACE_BOUNDS_UNAVAILABLE       = 7,
    // ...then the failures, in the spec's own numeric order so a code in a
    // guest log can be found by scanning
    KLXR_ERROR_VALIDATION_FAILURE       = -1,
    KLXR_ERROR_RUNTIME_FAILURE          = -2,
    KLXR_ERROR_FUNCTION_UNSUPPORTED     = -7,
    KLXR_ERROR_FEATURE_UNSUPPORTED      = -8,
    KLXR_ERROR_EXTENSION_NOT_PRESENT    = -9,
    KLXR_ERROR_LIMIT_REACHED            = -10,
    KLXR_ERROR_SIZE_INSUFFICIENT        = -11,
    KLXR_ERROR_HANDLE_INVALID           = -12,
    KLXR_ERROR_SESSION_RUNNING          = -14,
    KLXR_ERROR_SESSION_NOT_RUNNING      = -16,
    KLXR_ERROR_SYSTEM_INVALID           = -18,
    KLXR_ERROR_PATH_INVALID             = -19,
    KLXR_ERROR_PATH_COUNT_EXCEEDED      = -20,
    KLXR_ERROR_PATH_FORMAT_INVALID      = -21,
    KLXR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED = -26,
    KLXR_ERROR_SESSION_NOT_READY        = -28,
    KLXR_ERROR_SESSION_NOT_STOPPING     = -29,
    KLXR_ERROR_REFERENCE_SPACE_UNSUPPORTED = -31,
    KLXR_ERROR_FORM_FACTOR_UNSUPPORTED  = -34,
    KLXR_ERROR_CALL_ORDER_INVALID       = -37,
    KLXR_ERROR_GRAPHICS_DEVICE_INVALID  = -38,
    KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED = -41,
    KLXR_ERROR_ACTIONSET_NOT_ATTACHED   = -46,
    KLXR_ERROR_ACTIONSETS_ALREADY_ATTACHED = -47,
    KLXR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING = -50,
};

enum { KLXR_FORM_FACTOR_HMD = 1 };

// The "is this real" bits on a located space or view. VALID says the value is
// meaningful; TRACKED says it is being actively measured rather than inferred.
// Reporting VALID without TRACKED is how a runtime says "this is my best guess
// while tracking is lost", and an app is entitled to dim or freeze on it.
enum { KLXR_SPACE_ORIENTATION_VALID = 0x1, KLXR_SPACE_POSITION_VALID = 0x2,
       KLXR_SPACE_ORIENTATION_TRACKED = 0x4, KLXR_SPACE_POSITION_TRACKED = 0x8 };
enum { KLXR_VIEW_ORIENTATION_VALID = 0x1, KLXR_VIEW_POSITION_VALID = 0x2,
       KLXR_VIEW_ORIENTATION_TRACKED = 0x4, KLXR_VIEW_POSITION_TRACKED = 0x8 };
enum { KLXR_VIEW_CONFIG_PRIMARY_STEREO = 2 };

// ---------------------------------------------------------------- the surface
// The forty-six names libvrlink_scene.so imports, grouped the way the API is:
// instance/system, session, spaces, swapchains, the frame loop, actions, and
// the two haptic calls. Keeping the groups visible matters because the guest
// works through them roughly in this order, so the group a refusal lands in
// says how far the boot got without reading the whole log.
#define KL_XR_ENTRY_POINTS(X)                                                  \
    /* instance and system */                                                  \
    X(xrGetInstanceProcAddr)                                                   \
    X(xrEnumerateInstanceExtensionProperties)                                  \
    X(xrEnumerateApiLayerProperties)                                           \
    X(xrCreateInstance) X(xrDestroyInstance)                                   \
    X(xrGetInstanceProperties) X(xrResultToString)                             \
    X(xrGetSystem) X(xrGetSystemProperties)                                    \
    X(xrEnumerateViewConfigurationViews)                                       \
    X(xrEnumerateViewConfigurations) X(xrGetViewConfigurationProperties)       \
    X(xrEnumerateEnvironmentBlendModes) X(xrStructureTypeToString)             \
    /* session lifecycle */                                                    \
    X(xrCreateSession) X(xrDestroySession)                                     \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession)                  \
    X(xrPollEvent)                                                             \
    /* spaces */                                                               \
    X(xrCreateReferenceSpace) X(xrDestroySpace)                                \
    X(xrGetReferenceSpaceBoundsRect) X(xrLocateSpace)                          \
    X(xrEnumerateReferenceSpaces)                                              \
    /* swapchains */                                                           \
    X(xrEnumerateSwapchainFormats)                                             \
    X(xrCreateSwapchain) X(xrDestroySwapchain)                                 \
    X(xrEnumerateSwapchainImages)                                              \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)                         \
    X(xrReleaseSwapchainImage)                                                 \
    /* the frame loop */                                                       \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews)              \
    /* actions (input) */                                                      \
    X(xrStringToPath) X(xrPathToString)                                        \
    X(xrCreateActionSet) X(xrDestroyActionSet)                                 \
    X(xrCreateAction) X(xrDestroyAction) X(xrCreateActionSpace)                \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets)        \
    X(xrGetCurrentInteractionProfile) X(xrSyncActions)                         \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat)                        \
    X(xrGetActionStatePose)                                                    \
    /* haptics */                                                              \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback)                           \
    /* extensions — NOT in any import list, reachable only through             \
       xrGetInstanceProcAddr, which is why the "not served" line in that       \
       function exists: it is the only way one of these ever gets named. Each  \
       one below was added because that line named it and the guest then       \
       called the null pointer it got back. */                                 \
    X(xrInitializeLoaderKHR)                                                   \
    X(xrGetOpenGLESGraphicsRequirementsKHR)                                    \
    X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB)           \
    X(xrRequestDisplayRefreshRateFB)                                           \
    X(xrConvertTimespecTimeToTimeKHR) X(xrConvertTimeToTimespecTimeKHR)

// ------------------------------------------------------------------ bookkeeping
// One row per entry point: resolved counts lookups, called counts calls. The
// two are reported separately for the reason in the header — a name the guest
// resolved and never called is not a work item, and conflating them is how a
// work list doubles in size for no reason.
typedef struct { const char *name; void *fn; unsigned resolved, called; } klxr_row;

#define X(n) static int klxr_stub_##n(void);
KL_XR_ENTRY_POINTS(X)
#undef X

static klxr_row g_xr[] = {
#define X(n) { #n, (void *)klxr_stub_##n, 0, 0 },
    KL_XR_ENTRY_POINTS(X)
#undef X
};
#define KLXR_COUNT ((int)(sizeof g_xr / sizeof g_xr[0]))

static klxr_row *klxr_row_for(const char *name) {
    for (int i = 0; i < KLXR_COUNT; i++)
        if (strcmp(g_xr[i].name, name) == 0) return &g_xr[i];
    return NULL;
}

// The input surface's own half of the report, defined with the actions below.
// It is here rather than folded into the entry-point table because the question
// it answers is different in kind: the table says which calls arrived, and this
// says whether any of them ever carried a controller.
static void klxr_input_report(FILE *f);

void kl_openxr_report(FILE *f) {
    unsigned nres = 0, ncall = 0;
    for (int i = 0; i < KLXR_COUNT; i++) {
        nres  += g_xr[i].resolved != 0;
        ncall += g_xr[i].called   != 0;
    }
    fprintf(f, "\n=== OpenXR surface (libklepton_openxr) ===\n");
    fprintf(f, "  %d entry points served; %u resolved by the guest, "
               "%u refused by name\n", KLXR_COUNT, nres, ncall);
    if (ncall) {
        fprintf(f, "  --- refused (the work list, in refusal count order) ---\n");
        for (int i = 0; i < KLXR_COUNT; i++)
            if (g_xr[i].called)
                fprintf(f, "    %-42s x%u\n", g_xr[i].name, g_xr[i].called);
    }
    // The rest are the ones the guest looked up and we serve. `called` counts
    // REFUSALS only — klxr_unimplemented is the sole place it is bumped, because
    // an implemented entry point is dispatched straight through the pointer
    // xrGetInstanceProcAddr handed out and there is no seam left to count at.
    // This used to be headed "resolved but never called", which listed
    // xrEndFrame under it on a run whose own log shows xrEndFrame working —
    // a report that contradicts the trace costs more than no report (SL-13).
    int shown = 0;
    for (int i = 0; i < KLXR_COUNT; i++)
        if (g_xr[i].resolved && !g_xr[i].called) {
            if (!shown++)
                fprintf(f, "  --- resolved and served (no per-call count: "
                           "dispatched by pointer) ---\n");
            fprintf(f, "    %s\n", g_xr[i].name);
        }
    klxr_input_report(f);
}

// ------------------------------------------------------------- the refusal
// Everything not yet built. Named, so the guest says which of the forty-six it
// wants — the whole point of the surface existing at all.
//
// This aborts rather than returning an error code, and that is deliberate. An
// XrResult failure is a value the guest is entitled to handle, and it would:
// XrAppManager has error paths and would take one, land somewhere plausible,
// and the log would show a tidy shutdown with no mention of the function that
// was missing. That is precisely the "abort read as a crash" confusion in
// reverse, and it costs more than it saves.
static int klxr_unimplemented(klxr_row *row) {
    row->called++;
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented OpenXR entry "
                    "point '%s'\n", row->name);
    kl_openxr_report(stderr);
    kl_fatal_prepare();
    abort();
}

// The stubs themselves. Declared int(void) and called through whatever pointer
// type the guest has: AAPCS64 lets a callee ignore arguments it never reads,
// and none of these reads any.
#define X(n)                                                                   \
    static int klxr_stub_##n(void) {                                           \
        static klxr_row *row;                                                  \
        if (!row) row = klxr_row_for(#n);                                      \
        return klxr_unimplemented(row);                                        \
    }
KL_XR_ENTRY_POINTS(X)
#undef X

// --------------------------------------------------- what we do implement
//
// xrGetInstanceProcAddr, and it plays exactly the role the ovrp entry table
// plays in M6: our function handing out our own pointers. It is the one entry
// point a runtime must serve before anything else exists, because the spec has
// it work with `instance == XR_NULL_HANDLE` for the bootstrap trio
// (xrEnumerateInstanceExtensionProperties, xrEnumerateApiLayerProperties,
// xrCreateInstance) — so it cannot validate the instance, and we do not.
//
// A name we do not know is XR_ERROR_FUNCTION_UNSUPPORTED with *function left
// NULL, which is the specified answer and is a *measurement*, not a failure:
// the guest is entitled to ask about an extension entry point it will then not
// use. The assertion is still the call, and the stub it gets back is what
// makes that assertion by name.
static int klxr_GetInstanceProcAddr(void *instance, const char *name, void **function) {
    (void)instance;
    if (!function) return KLXR_ERROR_FUNCTION_UNSUPPORTED;
    *function = NULL;
    if (!name) return KLXR_ERROR_FUNCTION_UNSUPPORTED;

    klxr_row *row = klxr_row_for(name);
    if (!row) {
        // Printed once per distinct name and not counted, because it is neither
        // a resolution nor a call. This is the line that will name any
        // extension the guest wants — xrGetDisplayRefreshRateFB and friends —
        // and those never appear in the import list, so without it they would
        // be invisible until something else broke.
        fprintf(stderr, "  [xr] xrGetInstanceProcAddr(\"%s\") — not served\n", name);
        return KLXR_ERROR_FUNCTION_UNSUPPORTED;
    }
    row->resolved++;
    *function = row->fn;
    return KLXR_SUCCESS;
}

// xrInitializeLoaderKHR (XR_KHR_loader_init) — the first call this guest makes,
// before xrCreateInstance and before any extension has been enabled, which is
// what that extension is for. Its argument is an XrLoaderInitInfoAndroidKHR
// carrying `applicationVM` and `applicationContext`, and its entire purpose is
// to give the *Khronos loader* enough JNI to go and find a runtime through the
// Android broker. We ARE the runtime, so there is nothing to find and nothing
// we need from the struct — accepting is not a stub standing in for work, it is
// the work being already done.
//
// It has to be served, and the reason is worth keeping: the guest checks the
// result, LOGS the failure by name ("[CheckXR] Failed to call ... with error:
// -7") — and then calls the null function pointer anyway. So for this guest a
// refused proc-addr lookup is a SIGSEGV at 0x0 a moment later, with the honest
// diagnostic already printed above it. Any name that turns up in the "not
// served" line is therefore a crash waiting to happen, not a maybe.
static int klxr_InitializeLoaderKHR(const void *info) {
    (void)info;
    return KLXR_SUCCESS;
}

// ------------------------------------------------------------ the boot sequence
//
// Instance, system, view configuration. The guest walks these in order and each
// one's answer constrains the next, so they are written as one block: the
// system a form factor resolves to is the system the properties describe, and
// the view configuration is the one the eye textures will be sized from.
//
// **The eye size comes from kl_ovrp, not from a constant here.** That seam is
// already fed by the visionOS compositor's primeDisplay (it measures the
// drawable and pushes it), and it already carries the cap and the scale knob
// that stop a 6888x5525 logical eye turning into hundreds of MiB of swapchain.
// A second, parallel notion of "how big is an eye" is exactly the kind of thing
// that would disagree with the first one on device only.

// A handle is a pointer to one of these. The magic is what turns "the guest
// passed us a session where an instance goes" into a named refusal instead of a
// wild read — worth the four bytes, because every one of these calls takes a
// handle and the spec's own answer for a bad one (XR_ERROR_HANDLE_INVALID) is
// something the guest is equipped to log.
enum { KLXR_MAGIC_INSTANCE = 0x584b4c49 /* 'XKLI' */ };

typedef struct {
    uint32_t magic;
    XrVersion api_version;
    char      app_name[XR_MAX_APPLICATION_NAME_SIZE];
    char      engine_name[XR_MAX_ENGINE_NAME_SIZE];
    int       ext_opengl_es;      // did it enable XR_KHR_opengl_es_enable?
    int       gl_requirements_queried;   // ...and did it then ask for the range?
} klxr_instance;

static klxr_instance g_instance;

// systemId is a uint64_t the runtime picks; anything but XR_NULL_SYSTEM_ID (0)
// is legal, and a constant is right because there is exactly one HMD here.
enum { KLXR_SYSTEM_ID = 1 };

static klxr_instance *klxr_inst(void *h) {
    klxr_instance *i = (klxr_instance *)h;
    return (i && i->magic == KLXR_MAGIC_INSTANCE) ? i : NULL;
}

// The extensions we advertise, and the list is deliberately short.
//
// The guest's own log lines say what it does with the answer: it needs
// XR_KHR_opengl_es_enable ("XRQCreateXRSession: OpenGLES OpenXR Extension is
// not available" is a hard stop) and it *handles absence* of the rest —
// "XR_EXT_display_refresh_rate is not available on this runtime" is a line it
// prints and carries on from. So the honest answer to the twenty-odd FB/EXT
// names it knows about is that we do not have them, and every one of those is
// a feature we would otherwise have to implement to have claimed truthfully.
//
// XR_KHR_android_create_instance is here because this guest IS an Android
// activity and passes its VM and activity object through
// XrInstanceCreateInfoAndroidKHR; we already hold both.
static const struct { const char *name; uint32_t version; } g_extensions[] = {
    { "XR_KHR_opengl_es_enable",       10 },
    { "XR_KHR_android_create_instance", 3 },
    // XR_KHR_convert_timespec_time is the cheapest honest claim in this table:
    // our XrTime IS CLOCK_MONOTONIC nanoseconds (klxr_now), so the conversion is
    // arithmetic and cannot be wrong. Without it the guest logs "not available
    // on this runtime" on EVERY frame — a quarter of a million lines in a 40 s
    // run, which is not just noise: it is the guest's per-frame path doing
    // formatted I/O, and it buried the six lines of the video path that the run
    // existed to produce.
    { "XR_KHR_convert_timespec_time",   1 },
    // XR_FB_display_refresh_rate is the exception to the paragraph above, and
    // it is here because absence turned out NOT to be handled gracefully after
    // all (SL-11). The guest prints "XR_EXT_display_refresh_rate is not
    // available on this runtime" and carries on — but the thing it carried on
    // to was "[SVLClientXR] Supported refresh rates was empty!", and it
    // publishes that empty list to the Steam host as
    // VTE_AVAILABLE_FRAMETIMES_US. A host told the client can present at no
    // rate at all never starts sending video: the link came up, the control
    // channel exchanged updates, and the decoder was configured and then never
    // handed a single buffer.
    //
    // Claiming it is truthful because we can answer it truthfully: the display
    // frequency is measured (kl_ovrp_set_display_frequency, pushed by the
    // compositor's primeDisplay) and it is the SAME number the OVRP side
    // reports, so the two XR APIs cannot disagree about the panel — SL-9's
    // rule for eye poses, in the one other place a headset property is
    // answered twice.
    { "XR_FB_display_refresh_rate",     1 },
};
#define KLXR_EXT_ALL ((uint32_t)(sizeof g_extensions / sizeof g_extensions[0]))

// KL_XR_REFRESH_EXT=0 hides XR_FB_display_refresh_rate again — the A/B for the
// finding above, and it works by count because that extension is deliberately
// the last entry. Advertising it changes what the client publishes to the
// Steam host, so this is the knob that says whether a change in host behaviour
// came from here.
static uint32_t klxr_ext_count(void) {
    const char *e = getenv("KL_XR_REFRESH_EXT");
    return (e && !strcmp(e, "0")) ? KLXR_EXT_ALL - 1 : KLXR_EXT_ALL;
}
#define KLXR_EXT_COUNT (klxr_ext_count())

// The two-call idiom, which every enumerate in OpenXR uses and which is easy to
// get subtly wrong: capacityInput 0 means "just tell me the count" and must
// still be XR_SUCCESS, a capacity below the count is XR_ERROR_SIZE_INSUFFICIENT,
// and countOutput is written in ALL of those cases. Factored out so the four
// enumerators cannot disagree about it.
static XrResult klxr_two_call(uint32_t capacity, uint32_t *count_out, uint32_t have) {
    if (!count_out) return KLXR_ERROR_VALIDATION_FAILURE;
    *count_out = have;
    if (capacity == 0) return KLXR_SUCCESS;
    if (capacity < have) return KLXR_ERROR_SIZE_INSUFFICIENT;
    return KLXR_SUCCESS;
}

// Anything chained onto a struct we fill is a capability question. We answer
// the ones we know and say nothing into the others — but we PRINT them, once
// each, because a chained type is the guest naming a feature it is prepared to
// use, which is the same kind of measurement the "not served" line above makes.
// ONCE per (site, type). This is a capability question and the answer does not
// change between frames, but the sites are per-frame: xrLocateSpace chains an
// XrSpaceVelocity every frame, and at 90 Hz for 45 s that printed 95,773 lines —
// which is not merely noise, it is formatted I/O on the guest's frame path, and
// it buried the six lines the run existed to produce.
#define KLXR_CHAIN_SEEN 64
static void klxr_log_chain(const char *where, const void *next) {
    static struct { const char *where; int32_t type; } seen[KLXR_CHAIN_SEEN];
    static int n_seen;
    for (int depth = 0; next && depth < 16; depth++) {
        int32_t type = *(const int32_t *)next;
        int already = 0;
        for (int i = 0; i < n_seen; i++)
            if (seen[i].type == type && seen[i].where == where) { already = 1; break; }
        if (!already) {
            if (n_seen < KLXR_CHAIN_SEEN) seen[n_seen++] = (typeof(seen[0])){where, type};
            fprintf(stderr, "  [xr] %s: chained struct type %d — not filled in\n",
                    where, type);
        }
        next = *(const void *const *)((const char *)next + 8);
    }
}

// ...and the one chained struct that must NOT merely be logged. The guest asks
// for velocity alongside every pose, and an output struct we do not write is
// whatever its caller's stack held — so "not filled in" was handing the client
// a garbage velocity, which it publishes to SteamVR as the basis for pose
// prediction. The struct has a field that says "we do not know": velocityFlags
// == 0 means neither component is valid. Answering zero-and-VALID would be the
// worse lie, because it asserts something is stationary.
//
// `lin`/`ang` are 3 floats each in the BASE space's frame, or NULL for "no
// source". There is one for a controller — the Sense controllers report it and
// kl_ovrp carries it — and none for the head, which is why this takes them as
// arguments instead of deciding for itself.
//
// Offsets are the Khronos header's, transcribed: type 0, next 8, velocityFlags
// 16 (XrFlags64), linearVelocity 24, angularVelocity 36.
#define KLXR_TYPE_SPACE_VELOCITY 43
enum { KLXR_VELOCITY_LINEAR_VALID = 0x1, KLXR_VELOCITY_ANGULAR_VALID = 0x2 };
static void klxr_fill_space_velocity(void *next, const float *lin, const float *ang) {
    for (int depth = 0; next && depth < 16; depth++) {
        if (*(int32_t *)next == KLXR_TYPE_SPACE_VELOCITY) {
            char *v = next;
            uint64_t flags = 0;
            memset(v + 24, 0, 2 * 3 * sizeof(float));  // linear + angular
            if (lin) { memcpy(v + 24, lin, 3 * sizeof(float));
                       flags |= KLXR_VELOCITY_LINEAR_VALID; }
            if (ang) { memcpy(v + 36, ang, 3 * sizeof(float));
                       flags |= KLXR_VELOCITY_ANGULAR_VALID; }
            *(uint64_t *)(v + 16) = flags;
        }
        next = *(void **)((char *)next + 8);
    }
}

static XrResult klxr_EnumerateInstanceExtensionProperties(
        const char *layer_name, uint32_t capacity, uint32_t *count_out,
        XrExtensionProperties *props) {
    // A layer name we do not have is XR_ERROR_API_LAYER_NOT_PRESENT, but we
    // have no layers at all, so the only legal argument is NULL and anything
    // else is the guest asking about something that cannot exist here.
    if (layer_name && layer_name[0]) return KLXR_ERROR_VALIDATION_FAILURE;

    XrResult r = klxr_two_call(capacity, count_out, KLXR_EXT_COUNT);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!props) return KLXR_ERROR_VALIDATION_FAILURE;

    for (uint32_t i = 0; i < KLXR_EXT_COUNT; i++) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        snprintf(props[i].extensionName, sizeof props[i].extensionName,
                 "%s", g_extensions[i].name);
        props[i].extensionVersion = g_extensions[i].version;
    }
    return KLXR_SUCCESS;
}

// xrEnumerateApiLayerProperties — zero layers, and zero is the truth rather
// than a stand-in. API layers are a LOADER feature: the Khronos loader finds
// them on disk and interposes them between the app and the runtime. We replace
// that loader (see kl_openxr_dlopen), we interpose nothing, and there is no
// filesystem location here for a layer to be found in.
//
// It has to be SERVED even so, and Unity is what proved it: this is not an
// extension entry point a caller may skip, it is core OpenXR 1.0 that
// UnityOpenXR calls unconditionally as its first act, and it treats a failure
// as fatal. Answering XR_ERROR_FUNCTION_UNSUPPORTED — which is what an unknown
// name gets from klxr_GetInstanceProcAddr, correctly — threw straight out of
// Display_Initialize with the whole XR stack unstarted:
//
//   Error: XrResult failure [XR_ERROR_FUNCTION_UNSUPPORTED]
//       Origin: xrEnumerateApiLayerProperties(0, &layerCount, nullptr)
//
// It is also callable with no instance, like its sibling above, which is why
// klxr_GetInstanceProcAddr does not validate one.
static XrResult klxr_EnumerateApiLayerProperties(uint32_t capacity,
                                                 uint32_t *count_out, void *props) {
    (void)props;   // never written: the count is always zero
    return klxr_two_call(capacity, count_out, 0);
}


// ---- the "what do you support?" enumerators ---------------------------------
//
// Four two-call queries that a UNITY guest makes and Steam Link never did, and
// none of them is a decision: each one reports exactly what this runtime
// already implements elsewhere in this file, so the answer is a restatement
// rather than a new claim. Getting one to disagree with its implementation
// would be the worst kind of bug here — the guest would ask for the thing it
// was told about and be refused at creation time, several calls later.
//
// They come as a group because that is how UnityOpenXR's CreateSessionIfNeeded
// walks them, and because a guest that is told about a reference space it
// cannot create is in a worse state than one that stopped at the first name.

// Stereo, and only stereo — the same single configuration
// klxr_EnumerateViewConfigurationViews and klxr_BeginSession already refuse
// everything else in favour of.
static XrResult klxr_EnumerateViewConfigurations(void *instance, XrSystemId system_id,
                                                 uint32_t capacity, uint32_t *count_out,
                                                 int32_t *configs) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    XrResult r = klxr_two_call(capacity, count_out, 1);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!configs) return KLXR_ERROR_VALIDATION_FAILURE;
    configs[0] = KLXR_VIEW_CONFIG_PRIMARY_STEREO;
    return KLXR_SUCCESS;
}

// XrViewConfigurationProperties is { type, next, viewConfigurationType,
// fovMutable }. fovMutable is FALSE and that is load-bearing rather than
// conservative: it says the app may not change the field of view, and the
// frusta here are the display's own (kl_ovrp_eye_view / the compositor's
// measured tangents). A guest told it could mutate them would submit a picture
// rendered for a projection the compositor is not going to use.
#define KLXR_TYPE_VIEW_CONFIGURATION_PROPERTIES 42
static XrResult klxr_GetViewConfigurationProperties(void *instance, XrSystemId system_id,
                                                    int32_t view_config_type, void *props) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (view_config_type != KLXR_VIEW_CONFIG_PRIMARY_STEREO)
        return KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (!props) return KLXR_ERROR_VALIDATION_FAILURE;
    char *p = props;
    *(int32_t *)(p + 0)  = KLXR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    klxr_log_chain("xrGetViewConfigurationProperties", *(void **)(p + 8));
    *(int32_t *)(p + 16) = KLXR_VIEW_CONFIG_PRIMARY_STEREO;
    *(int32_t *)(p + 20) = 0;                     // fovMutable = XR_FALSE
    return KLXR_SUCCESS;
}

// OPAQUE only. The guest's picture covers the display and nothing of the room
// shows through it — which is what every compositor path in this project does
// (kl_reproject draws the eye texture as the whole scene). ADDITIVE and
// ALPHA_BLEND are the passthrough modes, and offering one would have the guest
// leave its background transparent for a blend that never happens: trap 33's
// failure, arrived at deliberately.
enum { KLXR_BLEND_OPAQUE = 1 };
static XrResult klxr_EnumerateEnvironmentBlendModes(void *instance, XrSystemId system_id,
                                                    int32_t view_config_type,
                                                    uint32_t capacity, uint32_t *count_out,
                                                    int32_t *modes) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (view_config_type != KLXR_VIEW_CONFIG_PRIMARY_STEREO)
        return KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    XrResult r = klxr_two_call(capacity, count_out, 1);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!modes) return KLXR_ERROR_VALIDATION_FAILURE;
    modes[0] = KLXR_BLEND_OPAQUE;
    return KLXR_SUCCESS;
}

// xrStructureTypeToString, the sibling of xrResultToString. Diagnostic only —
// the guest prints it — so the honest answer for a type we have no name for is
// the NUMBER, which is still enough to look up. XR_MAX_STRUCTURE_NAME_SIZE is
// 64 and the buffer is the caller's.
static XrResult klxr_StructureTypeToString(void *instance, int32_t value, char *buffer) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!buffer) return KLXR_ERROR_VALIDATION_FAILURE;
    snprintf(buffer, 64, "XR_TYPE_%d", value);
    return KLXR_SUCCESS;
}

static XrResult klxr_CreateInstance(const XrInstanceCreateInfo *info, void **instance) {
    if (!info || !instance) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_INSTANCE_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;

    // XrInstanceCreateInfoAndroidKHR rides in `next` and carries the VM and the
    // activity. We need neither — we made both — so this is logged and not read.
    klxr_log_chain("xrCreateInstance", info->next);

    memset(&g_instance, 0, sizeof g_instance);
    g_instance.magic = KLXR_MAGIC_INSTANCE;
    g_instance.api_version = info->applicationInfo.apiVersion;
    snprintf(g_instance.app_name, sizeof g_instance.app_name, "%s",
             info->applicationInfo.applicationName);
    snprintf(g_instance.engine_name, sizeof g_instance.engine_name, "%s",
             info->applicationInfo.engineName);

    // Every extension it asks to enable must be one we advertised — the spec
    // requires XR_ERROR_EXTENSION_NOT_PRESENT otherwise, and accepting silently
    // would be the worst of both: the guest would believe it had a feature and
    // find out through a null pointer.
    fprintf(stderr, "  [xr] xrCreateInstance: app=\"%s\" engine=\"%s\" api=%llu.%llu.%llu\n",
            g_instance.app_name, g_instance.engine_name,
            (unsigned long long)(g_instance.api_version >> 48) & 0xffff,
            (unsigned long long)(g_instance.api_version >> 32) & 0xffff,
            (unsigned long long)(g_instance.api_version & 0xffffffff));
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *name = info->enabledExtensionNames[i];
        int known = 0;
        for (uint32_t k = 0; k < KLXR_EXT_COUNT; k++)
            if (strcmp(name, g_extensions[k].name) == 0) known = 1;
        fprintf(stderr, "  [xr]   extension: %-40s %s\n", name,
                known ? "enabled" : "NOT PRESENT");
        if (!known) { g_instance.magic = 0; return KLXR_ERROR_EXTENSION_NOT_PRESENT; }
        if (strcmp(name, "XR_KHR_opengl_es_enable") == 0) g_instance.ext_opengl_es = 1;
    }

    *instance = &g_instance;
    return KLXR_SUCCESS;
}

static XrResult klxr_DestroyInstance(void *instance) {
    klxr_instance *inst = klxr_inst(instance);
    if (!inst) return KLXR_ERROR_HANDLE_INVALID;
    inst->magic = 0;
    return KLXR_SUCCESS;
}

static XrResult klxr_GetInstanceProperties(void *instance, XrInstanceProperties *props) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!props) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrGetInstanceProperties", props->next);
    props->type = XR_TYPE_INSTANCE_PROPERTIES;
    props->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    snprintf(props->runtimeName, sizeof props->runtimeName, "Klepton");
    return KLXR_SUCCESS;
}

static XrResult klxr_GetSystem(void *instance, const XrSystemGetInfo *info,
                               XrSystemId *system_id) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !system_id) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_SYSTEM_GET_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrGetSystem", info->next);
    // A handheld display is a phone, and this runtime is not one. Refusing it
    // is not a limitation we are apologising for — it is the answer that sends
    // a guest with a 2D fallback down the 2D path, which is the correct one for
    // a device that is not there.
    if (info->formFactor != KLXR_FORM_FACTOR_HMD)
        return KLXR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *system_id = KLXR_SYSTEM_ID;
    return KLXR_SUCCESS;
}

static XrResult klxr_GetSystemProperties(void *instance, XrSystemId system_id,
                                         XrSystemProperties *props) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (!props) return KLXR_ERROR_VALIDATION_FAILURE;
    // XrSystemHandTrackingPropertiesEXT and friends chain here, and leaving
    // them untouched is how we report the feature absent: the guest zeroes the
    // struct before the call, so an unwritten supported flag reads as false.
    klxr_log_chain("xrGetSystemProperties", props->next);

    int ew = 0, eh = 0;
    kl_ovrp_eye_texture_size(&ew, &eh);

    props->type = XR_TYPE_SYSTEM_PROPERTIES;
    props->systemId = system_id;
    props->vendorId = 0;
    snprintf(props->systemName, sizeof props->systemName, "Klepton HMD");
    // The maxima are a ceiling on what a swapchain may ask for, not a
    // recommendation — so they are generous, and the recommendation lives in
    // the view configuration below where the guest will actually read it.
    props->graphicsProperties.maxSwapchainImageWidth  = (uint32_t)(ew > 0 ? ew * 2 : 8192);
    props->graphicsProperties.maxSwapchainImageHeight = (uint32_t)(eh > 0 ? eh * 2 : 8192);
    props->graphicsProperties.maxLayerCount = 16;
    props->trackingProperties.orientationTracking = 1;
    props->trackingProperties.positionTracking = 1;
    return KLXR_SUCCESS;
}

static XrResult klxr_EnumerateViewConfigurationViews(
        void *instance, XrSystemId system_id, int32_t view_config_type,
        uint32_t capacity, uint32_t *count_out, XrViewConfigurationView *views) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    // Stereo is the only configuration we offer, and answering for mono would
    // be answering for a device that is not this one.
    if (view_config_type != KLXR_VIEW_CONFIG_PRIMARY_STEREO)
        return KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    XrResult r = klxr_two_call(capacity, count_out, 2);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!views) return KLXR_ERROR_VALIDATION_FAILURE;

    int ew = 0, eh = 0;
    kl_ovrp_eye_texture_size(&ew, &eh);
    if (ew <= 0 || eh <= 0) { ew = 2290; eh = 2400; }   // the Quest 2 default

    for (uint32_t i = 0; i < 2; i++) {
        klxr_log_chain("xrEnumerateViewConfigurationViews", views[i].next);
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        views[i].recommendedImageRectWidth  = (uint32_t)ew;
        views[i].recommendedImageRectHeight = (uint32_t)eh;
        views[i].maxImageRectWidth  = (uint32_t)ew * 2;
        views[i].maxImageRectHeight = (uint32_t)eh * 2;
        // One sample recommended: the guest resolves into the eye texture
        // itself, and MSAA there is the shape trap 12's VRR note is about —
        // a foveated pass whose resolve must stay physical-to-physical.
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    fprintf(stderr, "  [xr] view configuration: 2 views, recommended %dx%d each\n", ew, eh);
    return KLXR_SUCCESS;
}

// xrGetOpenGLESGraphicsRequirementsKHR (XR_KHR_opengl_es_enable) — the gate the
// spec puts in front of xrCreateSession: a session with a GL binding is
// XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING unless this was called first, so
// runtimes get to state a version range before an app commits to a context.
//
// The range we state is ANGLE's, because ANGLE is what is underneath: ES 3.0 as
// the floor (that is the context kl_egl actually creates — trap 9 is the
// standing reminder that the *description* being 3.2 does not make the context
// 3.2) and 3.2 as the ceiling we will answer queries for. Overstating the floor
// is the dangerous direction: an app told it needs 3.2 asks for 3.2, and gets a
// context whose 3.2 entry points resolve and then fail on use.
//
// Not in the import list. It arrives through xrGetInstanceProcAddr, which is
// exactly what that function's "not served" line was for — it named this, the
// guest called the null pointer it got back, and the run died at 0x0.
static XrResult klxr_GetOpenGLESGraphicsRequirementsKHR(
        void *instance, XrSystemId system_id,
        XrGraphicsRequirementsOpenGLESKHR *reqs) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (!reqs) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrGetOpenGLESGraphicsRequirementsKHR", reqs->next);
    reqs->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    reqs->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    reqs->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    g_instance.gl_requirements_queried = 1;
    return KLXR_SUCCESS;
}

// ---------------------------------------------------------- the session, and
// the state machine that is the actual content of this half.
//
// OpenXR does not let an app just start rendering. The runtime walks it forward
// — IDLE, READY, SYNCHRONIZED, VISIBLE, FOCUSED — by *posting events the app
// polls for*, and the app is only permitted to call xrBeginSession while it is
// in READY. So the interesting work here is not xrCreateSession, which is
// bookkeeping; it is that xrPollEvent has to volunteer transitions the guest is
// waiting on, in order, and a runtime that posts none leaves a correct app
// sitting in its event loop forever with nothing wrong anywhere.
//
// That is the same shape as the Choreographer in M4 and the looper in SL-8: on
// Android something else was driving the pump, and here the runtime IS the
// something else. Third time this class of bug has cost a session, so the
// transitions are queued eagerly rather than waiting for a frontend to say the
// headset is worn — we have no such signal on the host, and an idle guest looks
// exactly like a hung one.
enum { KLXR_MAGIC_SESSION = 0x584b4c53 /* 'XKLS' */ };
enum { KLXR_EVENT_QUEUE = 16 };

typedef struct {
    uint32_t magic;
    klxr_instance *instance;
    XrSystemId systemId;
    void *egl_display, *egl_config, *egl_context;   // the guest's own GL binding
    int   state;                  // the last state we POSTED, not the next one
    int   running;                // between xrBeginSession and xrEndSession
    int   exit_requested;
    int   action_sets_attached;   // xrAttachSessionActionSets is once, and final
    int      frame_begun;         // between xrBeginFrame and xrEndFrame
    int64_t  frame_predicted_time;// what the last xrWaitFrame promised
    uint64_t frames_waited, frames_ended, layers_ignored;
    // Pending events, in order. The payload is a state for a session-state
    // change and unused for everything else — this used to be a bare `int
    // queue[]` of states, and it grew a kind the moment a second event type
    // existed (the interaction profile changing, which is how an app learns a
    // controller appeared). A queue that can only carry one event type is a
    // queue that silently cannot deliver the second.
    struct { int kind, state; } queue[KLXR_EVENT_QUEUE];
    int   qhead, qcount;
} klxr_session;

enum { KLXR_EV_SESSION_STATE = 0, KLXR_EV_INTERACTION_PROFILE = 1 };

static klxr_session g_session;

static klxr_session *klxr_sess(void *h) {
    klxr_session *s = (klxr_session *)h;
    return (s && s->magic == KLXR_MAGIC_SESSION) ? s : NULL;
}

// ---------------------------------------------------------- XR_FB_display_refresh_rate
//
// One rate, and it is the one the display actually runs at. Offering a menu we
// cannot switch between would be a promise — the same reasoning, and the same
// single source, as ovrp_GetSystemDisplayAvailableFrequencies in kl_ovrp.c.
//
// The Steam host asks for a rate it prefers (90 Hz here); the client compares
// it against this list, picks what it can do, and tells the host. That
// negotiation is the whole point of the extension for this guest, and an empty
// list is not a neutral answer to it.
enum { KLXR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB = -1000101000 };

static XrResult klxr_EnumerateDisplayRefreshRatesFB(void *session, uint32_t capacity,
                                                    uint32_t *count_out, float *rates) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    XrResult r = klxr_two_call(capacity, count_out, 1);
    if (r == KLXR_SUCCESS && capacity >= 1 && rates)
        rates[0] = kl_ovrp_display_frequency();
    return r;
}

static XrResult klxr_GetDisplayRefreshRateFB(void *session, float *rate) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    if (!rate) return KLXR_ERROR_VALIDATION_FAILURE;
    *rate = kl_ovrp_display_frequency();
    return KLXR_SUCCESS;
}

// 0.0f is the spec's "give me the system default", which is the only rate we
// have anyway. Anything else has to match it: accepting a rate we do not run at
// would make the guest pace its frames against a clock that does not exist.
static XrResult klxr_RequestDisplayRefreshRateFB(void *session, float rate) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    float have = kl_ovrp_display_frequency();
    if (rate != 0.0f && fabsf(rate - have) > 0.5f) {
        fprintf(stderr, "  [xr] xrRequestDisplayRefreshRateFB(%.1f) — the display "
                        "runs at %.1f; refused\n", (double)rate, (double)have);
        return KLXR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    }
    return KLXR_SUCCESS;
}


// ---------------------------------------------------------- XR_KHR_convert_timespec_time
//
// Both directions, and they are pure arithmetic here because klxr_now() below
// defines XrTime as CLOCK_MONOTONIC nanoseconds — the same clock a timespec
// from clock_gettime(CLOCK_MONOTONIC) carries. On a runtime whose XrTime came
// off some other clock this would need the offset between the two; ours does
// not, and that is a property worth stating rather than a coincidence.
static XrResult klxr_ConvertTimespecTimeToTimeKHR(void *instance,
                                                  const struct timespec *ts,
                                                  int64_t *time_out) {
    (void)instance;
    if (!ts || !time_out) return KLXR_ERROR_VALIDATION_FAILURE;
    *time_out = (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
    return KLXR_SUCCESS;
}

static XrResult klxr_ConvertTimeToTimespecTimeKHR(void *instance, int64_t time,
                                                  struct timespec *ts_out) {
    (void)instance;
    if (!ts_out) return KLXR_ERROR_VALIDATION_FAILURE;
    // Floor division, so a negative XrTime does not land a nanosecond field
    // outside [0, 1e9) — which is a malformed timespec, not merely an odd one.
    int64_t sec = time / 1000000000LL, nsec = time % 1000000000LL;
    if (nsec < 0) { nsec += 1000000000LL; sec--; }
    ts_out->tv_sec  = (time_t)sec;
    ts_out->tv_nsec = (long)nsec;
    return KLXR_SUCCESS;
}

// XrTime is int64 nanoseconds on a monotonic clock the runtime chooses. It must
// be the SAME clock the guest's own timing uses, for the reason the
// Choreographer note in CLAUDE.md gives: two monotonic clocks differ by an
// offset, and a frame delta computed across the pair is that offset rather than
// a duration. CLOCK_MONOTONIC is what System.nanoTime() already answers here.
static int64_t klxr_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const char *klxr_state_name(int s) {
    switch (s) {
        case KLXR_SESSION_STATE_IDLE:         return "IDLE";
        case KLXR_SESSION_STATE_READY:        return "READY";
        case KLXR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case KLXR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case KLXR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case KLXR_SESSION_STATE_STOPPING:     return "STOPPING";
        case KLXR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case KLXR_SESSION_STATE_EXITING:      return "EXITING";
        default:                              return "UNKNOWN";
    }
}

static void klxr_post_event(klxr_session *s, int kind, int state) {
    if (s->qcount >= KLXR_EVENT_QUEUE) {
        // Dropping a transition would strand the guest in whatever state it was
        // last told about, so this is loud rather than silent. It should be
        // unreachable: nothing here queues more than three at a time.
        fprintf(stderr, "  [xr] event queue full, dropping %s\n",
                kind == KLXR_EV_SESSION_STATE ? klxr_state_name(state)
                                              : "interaction profile change");
        return;
    }
    s->queue[(s->qhead + s->qcount) % KLXR_EVENT_QUEUE] =
        (typeof(s->queue[0])){ kind, state };
    s->qcount++;
}

static void klxr_post_state(klxr_session *s, int state) {
    klxr_post_event(s, KLXR_EV_SESSION_STATE, state);
}

static XrResult klxr_CreateSession(void *instance, const XrSessionCreateInfo *info,
                                   void **session) {
    klxr_instance *inst = klxr_inst(instance);
    if (!inst) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !session) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_SESSION_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;

    // The graphics binding is chained, not a parameter, and its absence is a
    // distinct error from a bad one: a headless session (no binding at all) is
    // legal only with XR_MND_headless, which we do not offer.
    const XrGraphicsBindingOpenGLESAndroidKHR *gl = NULL;
    for (const void *n = info->next; n; ) {
        int32_t type = *(const int32_t *)n;
        if (type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR)
            gl = (const XrGraphicsBindingOpenGLESAndroidKHR *)n;
        else
            fprintf(stderr, "  [xr] xrCreateSession: chained struct type %d — ignored\n", type);
        n = *(const void *const *)((const char *)n + 8);
    }
    if (!gl) {
        fprintf(stderr, "  [xr] xrCreateSession: no graphics binding chained\n");
        return KLXR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (!inst->gl_requirements_queried)
        return KLXR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;

    memset(&g_session, 0, sizeof g_session);
    g_session.magic = KLXR_MAGIC_SESSION;
    g_session.instance = inst;
    g_session.systemId = info->systemId;
    g_session.egl_display = gl->display;
    g_session.egl_config  = gl->config;
    g_session.egl_context = gl->context;
    g_session.state = KLXR_SESSION_STATE_UNKNOWN;

    fprintf(stderr, "  [xr] xrCreateSession: EGLDisplay %p config %p context %p\n",
            gl->display, gl->config, gl->context);

    // IDLE then READY, immediately. On a real headset READY waits until the
    // thing is on someone's head; here there is nothing to wait for, and making
    // the guest wait would only be a faithful reproduction of an idle headset.
    klxr_post_state(&g_session, KLXR_SESSION_STATE_IDLE);
    klxr_post_state(&g_session, KLXR_SESSION_STATE_READY);

    *session = &g_session;
    return KLXR_SUCCESS;
}

static XrResult klxr_DestroySession(void *session) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    s->magic = 0;
    return KLXR_SUCCESS;
}

static XrResult klxr_BeginSession(void *session, const XrSessionBeginInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (s->running) return KLXR_ERROR_SESSION_RUNNING;
    if (s->state != KLXR_SESSION_STATE_READY) return KLXR_ERROR_SESSION_NOT_READY;
    if (info->primaryViewConfigurationType != KLXR_VIEW_CONFIG_PRIMARY_STEREO)
        return KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    klxr_log_chain("xrBeginSession", info->next);
    s->running = 1;
    // SYNCHRONIZED is "the app's frame loop is now ticking with the runtime's",
    // VISIBLE is "and its pictures are being shown", FOCUSED is "and it is
    // receiving input". Nothing here ever takes focus away, so all three go
    // out together and the guest's own handler walks them in order.
    klxr_post_state(s, KLXR_SESSION_STATE_SYNCHRONIZED);
    klxr_post_state(s, KLXR_SESSION_STATE_VISIBLE);
    klxr_post_state(s, KLXR_SESSION_STATE_FOCUSED);
    return KLXR_SUCCESS;
}

static XrResult klxr_EndSession(void *session) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->running) return KLXR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != KLXR_SESSION_STATE_STOPPING) return KLXR_ERROR_SESSION_NOT_STOPPING;
    s->running = 0;
    klxr_post_state(s, KLXR_SESSION_STATE_IDLE);
    klxr_post_state(s, s->exit_requested ? KLXR_SESSION_STATE_EXITING
                                         : KLXR_SESSION_STATE_READY);
    return KLXR_SUCCESS;
}

// The app asking to be let go. The runtime's job is to walk it out the same way
// it walked it in — STOPPING, so the app calls xrEndSession, and then EXITING.
static XrResult klxr_RequestExitSession(void *session) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->running) return KLXR_ERROR_SESSION_NOT_RUNNING;
    s->exit_requested = 1;
    klxr_post_state(s, KLXR_SESSION_STATE_STOPPING);
    return KLXR_SUCCESS;
}

// xrPollEvent — XR_EVENT_UNAVAILABLE (a SUCCESS code, 4, not an error) when the
// queue is empty, which is the normal answer most frames. Trap 10's warning
// applies in an unusual direction here: a *positive* result is still success,
// so a caller testing `result == XR_SUCCESS` rather than `>= 0` would read an
// empty queue as an event. That is the guest's business, but it is why this
// returns the specified code rather than an error.
static XrResult klxr_PollEvent(void *instance, XrEventDataBuffer *data) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!data) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_session *s = &g_session;
    if (s->magic != KLXR_MAGIC_SESSION || s->qcount == 0) return KLXR_EVENT_UNAVAILABLE;

    int kind = s->queue[s->qhead].kind, state = s->queue[s->qhead].state;
    s->qhead = (s->qhead + 1) % KLXR_EVENT_QUEUE;
    s->qcount--;

    if (kind == KLXR_EV_INTERACTION_PROFILE) {
        // XrEventDataInteractionProfileChanged is { type, next, session } and
        // carries no profile: it is a nudge to re-read
        // xrGetCurrentInteractionProfile per top-level path, which is where the
        // answer lives. An app that only re-reads on this event — and Steam
        // Link's XRInput is one — never learns a controller appeared without it.
        XrEventDataInteractionProfileChanged *ev =
            (XrEventDataInteractionProfileChanged *)data;
        ev->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        ev->next = NULL;
        ev->session = s;
        fprintf(stderr, "  [xr] interaction profile changed\n");
        return KLXR_SUCCESS;
    }

    s->state = state;
    XrEventDataSessionStateChanged *ev = (XrEventDataSessionStateChanged *)data;
    ev->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    ev->next = NULL;
    ev->session = s;
    ev->state = state;
    ev->time = klxr_now();
    fprintf(stderr, "  [xr] session state -> %s\n", klxr_state_name(state));
    return KLXR_SUCCESS;
}

// ------------------------------------------------------------------- spaces
//
// A space is a coordinate frame with a handle. There are two kinds and we serve
// both from one pool, because from here they differ only in what they are
// anchored to: a REFERENCE space is anchored to the world or the head, an
// ACTION space to whatever a pose action is tracking (a controller grip, an
// aim ray).
//
// **What is NOT here yet is xrLocateSpace's real answer**, and the reason is
// the standing rule rather than an oversight: locating a space against another
// is where the head pose enters, and the head pose has a per-frame LATCH in
// kl_ovrp that exists because reading it live mid-frame is precisely the bug
// behind the visionOS temporal doubling (PLANNING §12.19). Wiring a second
// consumer to the unlatched value would reintroduce it in a new place. So
// spaces are created and named here, and the locate lands with the frame loop
// that latches.
enum { KLXR_MAGIC_SPACE = 0x584b4c50 /* 'XKLP' */, KLXR_SPACE_MAX = 32 };

enum { KLXR_REF_SPACE_VIEW = 1, KLXR_REF_SPACE_LOCAL = 2, KLXR_REF_SPACE_STAGE = 3 };

typedef struct {
    uint32_t magic;
    klxr_session *session;
    int     reference_type;       // one of the three above, 0 for an action space
    XrPosef offset;               // poseInReferenceSpace / poseInActionSpace
    void   *action;               // action spaces only: what this is anchored to
    XrPath  subaction_path;       // ...and which hand of it
} klxr_space;

static klxr_space g_spaces[KLXR_SPACE_MAX];

static klxr_space *klxr_space_of(void *h) {
    klxr_space *s = (klxr_space *)h;
    if (!s || s->magic != KLXR_MAGIC_SPACE) return NULL;
    // A pointer with our magic that is not IN the pool is a guest bug we would
    // rather name than follow.
    if (s < g_spaces || s >= g_spaces + KLXR_SPACE_MAX) return NULL;
    return s;
}

static klxr_space *klxr_space_alloc(void) {
    for (int i = 0; i < KLXR_SPACE_MAX; i++)
        if (!g_spaces[i].magic) return &g_spaces[i];
    return NULL;
}

static const char *klxr_ref_space_name(int t) {
    switch (t) {
        case KLXR_REF_SPACE_VIEW:  return "VIEW";
        case KLXR_REF_SPACE_LOCAL: return "LOCAL";
        case KLXR_REF_SPACE_STAGE: return "STAGE";
        default:                   return "?";
    }
}

// Which spaces the guest actually asks to be located, and in what — recorded
// the first time each combination is seen, because at 90 Hz this can only be a
// census.
//
// The list of resolved entry points cannot answer this and reading it as if it
// could has cost time before (SL-14): a guest resolves plenty it never calls,
// and `xrLocateViews` appearing there says nothing about which space it passes.
// The distinction is the whole question. A guest that locates its views in VIEW
// space is asking "where are the eyes relative to the head" — an eye-to-head —
// and one that locates them in LOCAL is asking where they are in the world.
// Those two answers differ by the standing height, and answering the first with
// the second puts that height into the eye-to-head: a lever arm the head then
// swings around instead of turning in place.
//
// Returns 1 the first time a combination is seen, and nothing else: the caller
// prints the line, in one fprintf, once it also has the answer it gave. Split
// across the computation it would be split in the log too — the guest logs from
// several threads and one landed in the middle of the first version of this.
// ...once per (call, space, base), AND THEN RARELY AGAIN.
//
// Once was enough while the only question was WHICH spaces the guest asks
// about (SL-16 answered a whole arc with that census). It is not enough for the
// question that follows — whether the ANSWERS are right — because the first
// call happens before the head has moved, so every position in the census is
// 0 and a leak of the head's own position is indistinguishable from no leak.
// The repeat costs one line per pair per ~600 calls and is the difference
// between a census and a measurement. KL_XR_LOCATE_EVERY=0 turns it off.
static int klxr_locate_seen(const char *call, int sp_type, int base_type) {
    static struct { const char *call; int sp, base; unsigned n; } seen[16];
    static int n;
    static int every = -1;
    if (every < 0) every = kl_env_int("KL_XR_LOCATE_EVERY", 600);
    for (int i = 0; i < n; i++)
        if (seen[i].call == call && seen[i].sp == sp_type && seen[i].base == base_type)
            return every > 0 && ++seen[i].n % (unsigned)every == 0;
    if (n == (int)(sizeof seen / sizeof seen[0])) return 0;
    seen[n].call = call; seen[n].sp = sp_type; seen[n].base = base_type; seen[n].n = 0; n++;
    return 1;
}

// Quaternion and pose algebra. Locating one space in another is a rigid
// transform, and the moment VIEW is one of the two it is a transform with a
// rotation in it — which is why this is here rather than a subtraction.
static XrQuaternionf klxr_qconj(XrQuaternionf q) {
    return (XrQuaternionf){-q.x, -q.y, -q.z, q.w};
}

static XrQuaternionf klxr_qmul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

static XrVector3f klxr_qrot(XrQuaternionf q, XrVector3f v) {
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), the usual form that
    // needs no matrix and no normalisation.
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return (XrVector3f){
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx),
    };
}

// b expressed in a's frame: inverse(a) * b.
static XrPosef klxr_pose_rel(XrPosef a, XrPosef b) {
    XrQuaternionf inv = klxr_qconj(a.orientation);
    XrVector3f d = { b.position.x - a.position.x,
                     b.position.y - a.position.y,
                     b.position.z - a.position.z };
    XrPosef out;
    out.position = klxr_qrot(inv, d);
    out.orientation = klxr_qmul(inv, b.orientation);
    return out;
}

// ...and the other direction: a pose stated IN `base`, expressed in base's
// parent. klxr_pose_rel's inverse, and the composition klxr_space_pose already
// does for a space's own offset.
static XrPosef klxr_pose_apply(XrPosef base, XrPosef p) {
    XrVector3f r = klxr_qrot(base.orientation, p.position);
    XrPosef out;
    out.position = (XrVector3f){ base.position.x + r.x,
                                 base.position.y + r.y,
                                 base.position.z + r.z };
    out.orientation = klxr_qmul(base.orientation, p.orientation);
    return out;
}

// Where a space's origin is, as a pose in the tracking space kl_ovrp answers in.
//
// That space is floor-relative: y = 0 is the floor and the head stands at
// KL_OVRP_EYE_HEIGHT above it. So STAGE is the tracking space unchanged, LOCAL
// — whose origin OpenXR puts where the head started, at eye level — is that
// same space raised by the standing height, and VIEW *is the head*, pose and
// all.
//
// VIEW being a whole pose rather than a y displacement is the point of this
// function. It used to be a single float, with a comment asserting that VIEW
// "never appears here as a base" — and this guest passes VIEW as the base to
// xrLocateViews, which is how a client asks for its eye-to-head. Answered with
// a y of zero, the eye-to-head came back carrying the standing height, so the
// eye sat 1.7 m from the point it was rotated about and the head swung through
// a large arc instead of turning in place. The census above is what settles
// which spaces a guest actually asks about; do not assume it confines itself to
// the two static ones.
//
// Getting the static two wrong is not subtle in the end result and is very
// subtle here: an app that places its UI in LOCAL and is answered STAGE puts
// every panel on the floor.
// Which hand an ACTION space follows, and whether it is a GRIP or an AIM pose.
// Defined with the actions, below, because it has to look inside one. Returns
// -1 when the space is anchored to nothing we can locate.
static int klxr_action_space_hand(const klxr_space *sp, int *is_aim);

// The aim pose, as a rotation off the grip pose. **-35 degrees, measured on
// hardware, not derived here.**
//
// OpenXR gives a controller two poses: the GRIP (the hilt — where the hand is)
// and the AIM (the ray — where the user is pointing). Steam Link asks for both
// by name: `pamir-stream-pose` binds grip and `ui_pointer_pose` binds aim, so
// the in-headset UI pointer is the aim one, and with the two collapsed the
// laser leaves the hand at the hilt's angle instead of the pointing angle.
//
// SL-20 shipped this at 0, on the argument that KleptonControllers already
// builds a hilt frame whose -Z points along the direction the hilt points, so
// aim and grip nearly coincide for this input source. **That argument was
// wrong on hardware** — the same 35 degrees a Touch controller needs is needed
// here too — which is worth keeping because it was a plausible argument from a
// real property of the frontend, and the headset settled it in one A/B where
// no amount of reading the basis conversion would have.
//
// Sign convention: R_x(θ) takes the forward vector (0,0,-1) to (0, sinθ,
// -cosθ), so positive pitches forward UP and negative pitches it down.
//
// +35 on the grip, confirmed by eye on a headset streaming from SteamVR.
//
// **The guest's own controller_config.json does NOT predict this sign, and it
// looks like it should.** Its per-profile hilt rotations are all negative about
// the same axis (-20.6 Touch, -10 Pico, -5 Vive), which is why -35 was tried
// first and was wrong by twice the angle. Those are the guest's
// grip-to-*device* offsets, applied on its side to a pose it already has; this
// is the correction from the frontend's hilt frame INTO the grip pose the guest
// expects, and the two run opposite ways. Do not re-derive the sign from that
// table — it is a plausible source that gives the wrong answer.
//
// **This is the OpenXR path only, so it does not touch Beat Saber**, which
// speaks OVRPlugin and never resolves a single xr* entry point (its own
// end-of-run report reads `0 resolved by the guest`). The rotation is applied
// to kl_openxr's local copy of the pose, not written back into kl_ovrp, so the
// two guests cannot be made to disagree by this knob. If this ever needs to
// move into kl_ovrp, it needs a per-guest split at that moment — the shared
// seam is the reason, not the knob.
#define KLXR_GRIP_PITCH_DEFAULT (37.0f)
#define KLXR_AIM_PITCH_DEFAULT  (0.0f)
static void klxr_pitch_about_x(XrPosef *p, float degrees) {
    if (degrees == 0.0f) return;
    float half = degrees * 0.5f * 3.14159265358979f / 180.0f;
    XrQuaternionf rx = { sinf(half), 0, 0, cosf(half) };
    p->orientation = klxr_qmul(p->orientation, rx);
}

// The two corrections, applied to every action-space pose. They are separate
// because they answer different questions, and collapsing them into one knob is
// exactly the mistake this code made first time round.
//
//   KL_XR_GRIP_PITCH  corrects the CONTROLLER — the pose bound to
//                     .../input/grip/pose, which for Steam Link is
//                     `pamir-stream-pose`, i.e. the hilt SteamVR renders and
//                     streams back. This is the one that visibly rotates the
//                     controller. It applies to aim spaces too, because an aim
//                     ray built on a mis-pitched hilt is mis-pitched with it.
//
//   KL_XR_AIM_PITCH   is the EXTRA offset between the aim ray and the grip,
//                     applied only to .../input/aim/pose (`ui_pointer_pose`,
//                     the in-headset UI pointer). Zero by default: the real
//                     aim-vs-grip angle of this input source has not been
//                     measured, and the frontend's hilt frame already points
//                     roughly where a hand points.
//
// **The first version applied the whole thing to the aim pose only**, which is
// why setting KL_XR_AIM_PITCH appeared to do nothing at all: the controller a
// user is looking at is the GRIP pose, and nothing touched it. A knob whose
// name matched the symptom but not the pose is worse than no knob — it reads as
// "the rotation is not the problem" when the rotation was never applied.
// Read once and SAY so, and the saying is why this is split out rather than
// being a lazy init inside the corrector: the corrector only runs when a hand
// is actually being tracked, so on any run without a frontend — every host run
// — it never executes and the log never said which pitch was in force. A knob
// whose value cannot be confirmed from the log is half a knob, and this pair is
// specifically the one a person A/Bs against a picture.
//
// Called from xrCreateActionSpace, which happens whenever a guest uses
// controllers at all, tracked or not.
static void klxr_pitches(float *grip, float *aim) {
    // A separate `init` flag rather than a sentinel value: the whole range of
    // both knobs is meaningful, negative included, so -1 cannot mean "not read
    // yet" without silently swallowing a legitimate setting.
    static int init;
    static float grip_pitch, aim_pitch;
    if (!init) {
        init = 1;
        grip_pitch = kl_env_float("KL_XR_GRIP_PITCH", KLXR_GRIP_PITCH_DEFAULT);
        aim_pitch  = kl_env_float("KL_XR_AIM_PITCH", KLXR_AIM_PITCH_DEFAULT);
        fprintf(stderr, "  [xr] controller pose: grip pitched %.1f deg "
                        "(KL_XR_GRIP_PITCH), aim a further %.1f deg "
                        "(KL_XR_AIM_PITCH)\n",
                (double)grip_pitch, (double)aim_pitch);
    }
    if (grip) *grip = grip_pitch;
    if (aim)  *aim  = aim_pitch;
}

static void klxr_pose_corrections(XrPosef *p, int is_aim) {
    float grip_pitch, aim_pitch;
    klxr_pitches(&grip_pitch, &aim_pitch);
    klxr_pitch_about_x(p, grip_pitch);
    if (is_aim) klxr_pitch_about_x(p, aim_pitch);
}

static XrPosef klxr_space_pose_ex(const klxr_space *sp, int *tracked,
                                  float *lin, float *ang) {
    XrPosef base = { {0, 0, 0, 1}, {0, 0, 0} };   // STAGE, and the tracking space
    if (tracked) *tracked = 1;
    if (!sp) return base;
    if (sp->reference_type == KLXR_REF_SPACE_VIEW) {
        kl_ovrp_get_guest_head_pose(&base.position.x, &base.position.y,
                                    &base.position.z, &base.orientation.x,
                                    &base.orientation.y, &base.orientation.z,
                                    &base.orientation.w);
    } else if (sp->reference_type == KLXR_REF_SPACE_LOCAL) {
        base.position.y = kl_ovrp_eye_height();
    } else if (sp->reference_type == 0) {
        // An ACTION space — anchored to whatever its pose action is following,
        // which here is one of the two hands. The pose and its motion come out
        // of the same latched sample the ovrp guest sees, and out of one call,
        // so a velocity can never be paired with another frame's pose.
        int is_aim = 0;
        int hand = klxr_action_space_hand(sp, &is_aim);
        float pos[3], quat[4], v[3], a[3];
        int present = hand >= 0 && kl_ovrp_hand_motion(hand, pos, quat, v, a);
        if (tracked) *tracked = present;
        if (!present) return base;
        base.position    = (XrVector3f){ pos[0], pos[1], pos[2] };
        base.orientation = (XrQuaternionf){ quat[0], quat[1], quat[2], quat[3] };
        klxr_pose_corrections(&base, is_aim);
        if (lin) memcpy(lin, v, sizeof v);
        if (ang) memcpy(ang, a, sizeof a);
    }

    // ...and the pose the guest asked its space to sit at within that reference
    // space. Every space this guest creates is at the identity, so composing it
    // costs nothing here — but a field we store and never read is a trap for
    // the next guest, and a recentre is exactly the sort of thing a client sets
    // once and then relies on for the rest of the session.
    XrPosef out;
    XrVector3f off = klxr_qrot(base.orientation, sp->offset.position);
    out.position = (XrVector3f){ base.position.x + off.x,
                                 base.position.y + off.y,
                                 base.position.z + off.z };
    out.orientation = klxr_qmul(base.orientation, sp->offset.orientation);
    return out;
}

// The pose alone, for every caller that only ever asks about a reference space
// — xrLocateViews and the self-test — where "tracked" is not a question.
static XrPosef klxr_space_pose(const klxr_space *sp) {
    return klxr_space_pose_ex(sp, NULL, NULL, NULL);
}

// The three reference spaces klxr_space_pose can place, and no others —
// xrCreateReferenceSpace refuses everything outside this set, so this list and
// that check are the same fact.
static XrResult klxr_EnumerateReferenceSpaces(void *session, uint32_t capacity,
                                              uint32_t *count_out, int32_t *spaces) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    static const int32_t have[] = { KLXR_REF_SPACE_VIEW, KLXR_REF_SPACE_LOCAL,
                                    KLXR_REF_SPACE_STAGE };
    const uint32_t n = sizeof have / sizeof have[0];
    XrResult r = klxr_two_call(capacity, count_out, n);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!spaces) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(spaces, have, sizeof have);
    return KLXR_SUCCESS;
}

static XrResult klxr_CreateReferenceSpace(void *session,
                                          const XrReferenceSpaceCreateInfo *info,
                                          void **space) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !space) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
        return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrCreateReferenceSpace", info->next);

    // VIEW, LOCAL and STAGE are the three every runtime must offer. The vendor
    // ones (UNBOUNDED_MSFT and friends) are extensions we do not advertise, and
    // refusing by name here is what makes a guest that wanted one say so.
    if (info->referenceSpaceType != KLXR_REF_SPACE_VIEW &&
        info->referenceSpaceType != KLXR_REF_SPACE_LOCAL &&
        info->referenceSpaceType != KLXR_REF_SPACE_STAGE) {
        fprintf(stderr, "  [xr] xrCreateReferenceSpace: type %d unsupported\n",
                info->referenceSpaceType);
        return KLXR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }

    klxr_space *sp = klxr_space_alloc();
    if (!sp) return KLXR_ERROR_LIMIT_REACHED;
    sp->magic = KLXR_MAGIC_SPACE;
    sp->session = s;
    sp->reference_type = info->referenceSpaceType;
    sp->offset = info->poseInReferenceSpace;
    fprintf(stderr, "  [xr] reference space %s at (%.3f %.3f %.3f)\n",
            klxr_ref_space_name(sp->reference_type),
            sp->offset.position.x, sp->offset.position.y, sp->offset.position.z);
    *space = sp;
    return KLXR_SUCCESS;
}

static XrResult klxr_DestroySpace(void *space) {
    klxr_space *sp = klxr_space_of(space);
    if (!sp) return KLXR_ERROR_HANDLE_INVALID;
    memset(sp, 0, sizeof *sp);
    return KLXR_SUCCESS;
}

// The play area, in metres. XR_SPACE_BOUNDS_UNAVAILABLE is a SUCCESS code (7),
// and it is the honest answer: there is no guardian here, nobody has drawn a
// boundary, and inventing one would be inventing a room. An app that gets this
// treats the stage as unbounded, which is what a seated Vision Pro user has.
static XrResult klxr_GetReferenceSpaceBoundsRect(void *session, int32_t ref_type,
                                                 XrExtent2Df *bounds) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    if (!bounds) return KLXR_ERROR_VALIDATION_FAILURE;
    (void)ref_type;
    bounds->width = bounds->height = 0.0f;
    return KLXR_SPACE_BOUNDS_UNAVAILABLE;
}

// ------------------------------------------------------------------ actions
//
// OpenXR does not let an app read a button. It declares *actions* ("teleport",
// "grip pose"), suggests bindings from those actions to concrete paths on named
// controllers, and the runtime decides what is actually wired. That indirection
// is why this block is mostly bookkeeping — and it is also why it is worth
// building even before there is any input to report: the suggested bindings the
// guest hands us ARE its input map, printed once, which is the measurement of
// what a frontend would eventually have to supply.
//
// **The action states are answered from the SAME input kl_ovrp already holds**
// — the poses and buttons M7's frontends publish (`KleptonControllers.swift` on
// device, `kl_view.c` on the host), read back through `kl_ovrp_hand_motion` /
// `kl_ovrp_controller_input`. One frontend, one sample of one instant, two XR
// APIs reading it: the same rule `kl_ovrp_eye_view` exists for.
//
// **The map from an action to a control is the GUEST'S OWN, not a table of
// ours.** `xrSuggestInteractionProfileBindings` hands us the whole thing —
// every action paired with the concrete path it expects to be driven from —
// and `steamlink-vr/assets/config/controller_config.json` is where the guest
// reads it from, so it is auditable offline. Keying on the guest's *action
// names* instead would have been the shorter code and would break on the next
// build that renames one; keying on the binding path breaks only if the
// controller changes, which is the thing the path is for.
//
// So the only judgement here is the decode from a path suffix to a field of
// kl_ovrp's per-hand state, and a path we do not recognise leaves that hand
// **unbound** — which reads as `isActive = false`, the defined answer for "the
// runtime bound nothing to this". That matters: `/input/thumbrest/touch` is a
// capacitive sensor no Vision Pro input source has, and reporting it as
// permanently-not-touched would be a measurement we cannot make, where
// inactive is the truth.
enum { KLXR_MAGIC_ACTION_SET = 0x584b4c41 /* 'XKLA' */,
       KLXR_MAGIC_ACTION     = 0x584b4c61 /* 'XKLa' */ };
enum { KLXR_PATH_MAX = 256, KLXR_ACTION_SET_MAX = 16, KLXR_ACTION_MAX = 128 };

enum { KLXR_ACTION_TYPE_BOOLEAN = 1, KLXR_ACTION_TYPE_FLOAT = 2,
       KLXR_ACTION_TYPE_VECTOR2F = 3, KLXR_ACTION_TYPE_POSE = 4,
       KLXR_ACTION_TYPE_VIBRATION = 100 };

// Which interaction profile we report as bound.
//
// It is not a free choice and it is not "none". Steam Link looks its controller
// descriptions up in controller_config.json's `staticProps` table **keyed by
// the active profile**, and says so by name when it fails: `[XRInput] Couldn't
// find static props for active interaction profile: %lu`. Answering XR_NULL_PATH
// is what left VTE_PROPS_STATIC_L/_R unpublished — the same shape as the
// `delmar` -> `hollywood` correction (SL-12), a lookup keyed on a device
// identity we answered wrong, breaking a feature two subsystems away.
//
// Touch is also the CONSISTENT answer rather than merely a plausible one: this
// shim presents a Quest 2 everywhere else (Build.MODEL, Build.PRODUCT
// "hollywood", ovrp_GetSystemHeadsetType), and the guest's own oculus entry
// reads "Oculus Quest2 (Left Controller)" / "oculus_touch". Answering a
// different profile would be the inconsistent act.
#define KLXR_ACTIVE_PROFILE "/interaction_profiles/oculus/touch_controller"

// What one binding path reads, once decoded.
enum {
    KLXR_SRC_NONE = 0,
    KLXR_SRC_BUTTON,          // a bit of the RAW buttons word
    KLXR_SRC_TOUCH,           // ...of the RAW touches word
    KLXR_SRC_INDEX_TRIGGER,
    KLXR_SRC_HAND_TRIGGER,
    KLXR_SRC_STICK_X,
    KLXR_SRC_STICK_Y,
    KLXR_SRC_POSE,            // .../input/grip/pose — the hilt
    KLXR_SRC_POSE_AIM,        // .../input/aim/pose  — the ray
    KLXR_SRC_HAPTIC,
};

// The decode table, and the whole of the judgement in this file's input path.
//
// `bit[hand]` is the ovrpButton/ovrpTouch RAW bit for the named control on that
// hand (kl_ovrp.h has the enum and why it must be raw). Zero means that hand
// does not have this control — A/B are the RIGHT controller's face buttons and
// X/Y the LEFT's, which is why they are separate rows rather than one aliased
// pair, and why a guest binding `/user/hand/left/input/a/click` would correctly
// come out unbound.
//
// Deliberately absent, and each absence is a measurement we cannot make rather
// than an oversight:
//   /input/thumbrest/touch  — no capacitive thumbrest on any source here
//   /input/squeeze/click    — vive_focus3 only, so never in the active profile
//   /input/trigger/click    — the same
static const struct { const char *suffix; int kind; uint32_t bit[2]; }
g_xr_sources[] = {
    { "/input/a/click",          KLXR_SRC_BUTTON, { 0, KL_OVRP_RAW_A } },
    { "/input/b/click",          KLXR_SRC_BUTTON, { 0, KL_OVRP_RAW_B } },
    { "/input/x/click",          KLXR_SRC_BUTTON, { KL_OVRP_RAW_X, 0 } },
    { "/input/y/click",          KLXR_SRC_BUTTON, { KL_OVRP_RAW_Y, 0 } },
    { "/input/menu/click",       KLXR_SRC_BUTTON, { KL_OVRP_RAW_START,
                                                    KL_OVRP_RAW_START } },
    { "/input/system/click",     KLXR_SRC_BUTTON, { KL_OVRP_RAW_BACK,
                                                    KL_OVRP_RAW_BACK } },
    { "/input/thumbstick/click", KLXR_SRC_BUTTON, { KL_OVRP_RAW_LTHUMBSTICK,
                                                    KL_OVRP_RAW_RTHUMBSTICK } },
    { "/input/a/touch",          KLXR_SRC_TOUCH,  { 0, KL_OVRP_RAW_A } },
    { "/input/b/touch",          KLXR_SRC_TOUCH,  { 0, KL_OVRP_RAW_B } },
    { "/input/x/touch",          KLXR_SRC_TOUCH,  { KL_OVRP_RAW_X, 0 } },
    { "/input/y/touch",          KLXR_SRC_TOUCH,  { KL_OVRP_RAW_Y, 0 } },
    { "/input/trigger/touch",    KLXR_SRC_TOUCH,  { KL_OVRP_RAW_LINDEX_TRIGGER,
                                                    KL_OVRP_RAW_RINDEX_TRIGGER } },
    { "/input/thumbstick/touch", KLXR_SRC_TOUCH,  { KL_OVRP_RAW_LTHUMBSTICK,
                                                    KL_OVRP_RAW_RTHUMBSTICK } },
    { "/input/trigger/value",    KLXR_SRC_INDEX_TRIGGER, { 0, 0 } },
    { "/input/squeeze/value",    KLXR_SRC_HAND_TRIGGER,  { 0, 0 } },
    { "/input/thumbstick/x",     KLXR_SRC_STICK_X, { 0, 0 } },
    { "/input/thumbstick/y",     KLXR_SRC_STICK_Y, { 0, 0 } },
    { "/input/grip/pose",        KLXR_SRC_POSE,     { 0, 0 } },
    { "/input/aim/pose",         KLXR_SRC_POSE_AIM, { 0, 0 } },
    { "/output/haptic",          KLXR_SRC_HAPTIC, { 0, 0 } },
};
#define KLXR_SOURCE_COUNT ((int)(sizeof g_xr_sources / sizeof g_xr_sources[0]))

// The two top-level user paths that exist in this guest's binary. A binding
// under anything else (a gamepad, a treadmill) has no hand and stays unbound.
static int klxr_path_hand(const char *p, const char **suffix) {
    static const struct { const char *prefix; int hand; } tops[] = {
        { "/user/hand/left",  0 },
        { "/user/hand/right", 1 },
    };
    for (int i = 0; i < 2; i++) {
        size_t n = strlen(tops[i].prefix);
        if (strncmp(p, tops[i].prefix, n) == 0) {
            if (suffix) *suffix = p + n;
            return tops[i].hand;
        }
    }
    return -1;
}

static const char *klxr_src_name(int kind) {
    switch (kind) {
        case KLXR_SRC_BUTTON:        return "button";
        case KLXR_SRC_TOUCH:         return "touch";
        case KLXR_SRC_INDEX_TRIGGER: return "index trigger";
        case KLXR_SRC_HAND_TRIGGER:  return "hand trigger";
        case KLXR_SRC_STICK_X:       return "thumbstick x";
        case KLXR_SRC_STICK_Y:       return "thumbstick y";
        case KLXR_SRC_POSE:          return "grip pose";
        case KLXR_SRC_POSE_AIM:      return "aim pose";
        case KLXR_SRC_HAPTIC:        return "haptic";
        default:                     return "unbound";
    }
}

// The path table. Interning is the whole of xrStringToPath: a path is an
// opaque uint64 the app compares for equality and hands back, so an index into
// this table is exactly as good as a hash and can be turned back into the
// string, which xrPathToString needs and a hash would not survive.
static char     g_paths[KLXR_PATH_MAX][XR_MAX_PATH_LENGTH];
static uint32_t g_path_count;

typedef struct { uint32_t magic; char name[XR_MAX_ACTION_SET_NAME_SIZE];
                 uint32_t priority; int attached; } klxr_action_set;
typedef struct {
    uint32_t magic;
    char     name[XR_MAX_ACTION_NAME_SIZE];
    int32_t  type;
    klxr_action_set *set;
    // What the guest's own suggested bindings said this action reads, per hand,
    // for the profile we report active. kind 0 = this hand is not bound.
    int      kind[2];
    uint32_t bit[2];
    XrPath   bind[2];
    // The per-frame SNAPSHOT. OpenXR's model is that action state is sampled at
    // xrSyncActions and read back unchanged for the rest of the frame — which
    // is not merely a permission, it is what makes changedSinceLastSync
    // meaningful at all. Reading live in the getters would let two reads in one
    // frame disagree, and would make "changed" a function of how often the
    // guest asked rather than of what the user did.
    float    value[2];
    int      active[2], changed[2];
    int64_t  change_time[2];
    // Three different questions the end-of-run report has to keep apart: was
    // this action ever READ, was it ever ACTIVE (a controller was there), and
    // did it ever carry a NON-ZERO value (someone actually did something). A
    // bound button on a live controller that nobody pressed is a healthy
    // action, and reporting it as "never active" would send the next session
    // looking for a bug in the binding.
    unsigned reads, active_reads, nonzero_reads;
    int      said;                  // the one-line "this went live" print
} klxr_action;

static klxr_action_set g_action_sets[KLXR_ACTION_SET_MAX];
static klxr_action     g_actions[KLXR_ACTION_MAX];
// Counters for the end-of-run report. `syncs` is the one that separates "the
// guest never asked" from "we never answered".
static struct { unsigned syncs, bound, hands_seen, haptic_pulses; } g_xr_input;

static XrResult klxr_StringToPath(void *instance, const char *path_string, XrPath *path) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!path_string || !path) return KLXR_ERROR_VALIDATION_FAILURE;
    // XR_ERROR_PATH_FORMAT_INVALID is for a string that is not a well-formed
    // path. We do not police the grammar — the guest's paths come from its own
    // tables, not from a user — but the length IS a limit we would silently
    // truncate past, and a truncated path would then compare equal to a
    // different one.
    if (strlen(path_string) >= XR_MAX_PATH_LENGTH) return KLXR_ERROR_PATH_FORMAT_INVALID;

    for (uint32_t i = 0; i < g_path_count; i++)
        if (strcmp(g_paths[i], path_string) == 0) { *path = i + 1; return KLXR_SUCCESS; }
    if (g_path_count >= KLXR_PATH_MAX) return KLXR_ERROR_PATH_COUNT_EXCEEDED;
    snprintf(g_paths[g_path_count], XR_MAX_PATH_LENGTH, "%s", path_string);
    *path = ++g_path_count;         // 1-based; 0 stays XR_NULL_PATH
    return KLXR_SUCCESS;
}

static const char *klxr_path_str(XrPath p) {
    if (p == 0 || p > g_path_count) return "<null path>";
    return g_paths[p - 1];
}

static XrResult klxr_PathToString(void *instance, XrPath path, uint32_t capacity,
                                  uint32_t *count_out, char *buffer) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (path == 0 || path > g_path_count) return KLXR_ERROR_PATH_INVALID;
    const char *s = g_paths[path - 1];
    uint32_t need = (uint32_t)strlen(s) + 1;         // the count INCLUDES the NUL
    XrResult r = klxr_two_call(capacity, count_out, need);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!buffer) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(buffer, s, need);
    return KLXR_SUCCESS;
}

static XrResult klxr_CreateActionSet(void *instance, const XrActionSetCreateInfo *info,
                                     void **action_set) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !action_set) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_ACTION_SET_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrCreateActionSet", info->next);
    for (int i = 0; i < KLXR_ACTION_SET_MAX; i++) {
        if (g_action_sets[i].magic) continue;
        g_action_sets[i].magic = KLXR_MAGIC_ACTION_SET;
        g_action_sets[i].priority = info->priority;
        snprintf(g_action_sets[i].name, sizeof g_action_sets[i].name, "%s",
                 info->actionSetName);
        fprintf(stderr, "  [xr] action set \"%s\" (priority %u)\n",
                g_action_sets[i].name, info->priority);
        *action_set = &g_action_sets[i];
        return KLXR_SUCCESS;
    }
    return KLXR_ERROR_LIMIT_REACHED;
}

// Both pools are validated the same way, and by IDENTITY rather than by magic
// alone: a magic word proves the guest handed back something we wrote, an
// address in the pool proves it is still one of ours.
static klxr_action_set *klxr_action_set_of(void *h) {
    for (int i = 0; i < KLXR_ACTION_SET_MAX; i++)
        if (h == &g_action_sets[i] && g_action_sets[i].magic) return &g_action_sets[i];
    return NULL;
}
static klxr_action *klxr_action_of(void *h) {
    for (int i = 0; i < KLXR_ACTION_MAX; i++)
        if (h == &g_actions[i] && g_actions[i].magic) return &g_actions[i];
    return NULL;
}

static XrResult klxr_DestroyActionSet(void *action_set) {
    klxr_action_set *h = klxr_action_set_of(action_set);
    if (!h) return KLXR_ERROR_HANDLE_INVALID;
    memset(h, 0, sizeof *h);
    return KLXR_SUCCESS;
}

static XrResult klxr_CreateAction(void *action_set, const XrActionCreateInfo *info,
                                  void **action) {
    klxr_action_set *set = klxr_action_set_of(action_set);
    if (!set) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !action) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_ACTION_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrCreateAction", info->next);
    for (int i = 0; i < KLXR_ACTION_MAX; i++) {
        if (g_actions[i].magic) continue;
        g_actions[i].magic = KLXR_MAGIC_ACTION;
        g_actions[i].type = info->actionType;
        g_actions[i].set = set;
        snprintf(g_actions[i].name, sizeof g_actions[i].name, "%s", info->actionName);
        *action = &g_actions[i];
        return KLXR_SUCCESS;
    }
    return KLXR_ERROR_LIMIT_REACHED;
}

static XrResult klxr_DestroyAction(void *action) {
    klxr_action *h = klxr_action_of(action);
    if (!h) return KLXR_ERROR_HANDLE_INVALID;
    memset(h, 0, sizeof *h);
    return KLXR_SUCCESS;
}

// An action space is a space anchored to a pose action — the grip or the aim
// ray of whichever controller the action bound to. It goes in the same pool as
// the reference spaces with reference_type 0, because the only thing that
// distinguishes them here is what xrLocateSpace will eventually ask about.
static XrResult klxr_CreateActionSpace(void *session,
                                       const XrActionSpaceCreateInfo *info,
                                       void **space) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !space) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    if (!klxr_action_of(info->action)) return KLXR_ERROR_HANDLE_INVALID;
    klxr_log_chain("xrCreateActionSpace", info->next);

    klxr_space *sp = klxr_space_alloc();
    if (!sp) return KLXR_ERROR_LIMIT_REACHED;
    sp->magic = KLXR_MAGIC_SPACE;
    sp->session = s;
    sp->reference_type = 0;
    sp->offset = info->poseInActionSpace;
    sp->action = info->action;
    sp->subaction_path = info->subactionPath;
    // The offset is not decorative here and this is the one place it is
    // visible: Steam Link's controller_config.json carries a per-profile grip
    // offset (-0.007, -0.034, -0.096 and -20.6 deg of pitch for Touch) and
    // hands it over as poseInActionSpace. It is composed in klxr_space_pose_ex
    // like any other; printing it is how a wrong hilt angle gets traced to the
    // guest's own table rather than to our basis.
    klxr_pitches(NULL, NULL);        // so the log always says which pitch is in force
    fprintf(stderr, "  [xr] action space for \"%s\" on %s at "
                    "(%.3f %.3f %.3f)\n",
            klxr_action_of(info->action)->name,
            info->subactionPath ? klxr_path_str(info->subactionPath) : "either hand",
            sp->offset.position.x, sp->offset.position.y, sp->offset.position.z);
    *space = sp;
    return KLXR_SUCCESS;
}

// Which hand an action space follows. The subaction path is the direct answer
// when the guest gave one; when it did not, the space follows whichever single
// hand its action is bound to, and a pose action bound to both with no
// subaction path is genuinely ambiguous — -1, and it locates untracked rather
// than picking the left one and being subtly wrong for one hand.
static int klxr_action_space_hand(const klxr_space *sp, int *is_aim) {
    if (is_aim) *is_aim = 0;
    if (!sp || sp->reference_type != 0) return -1;
    klxr_action *a = klxr_action_of(sp->action);
    if (!a) return -1;
    int hand = -1;
    if (sp->subaction_path) {
        hand = klxr_path_hand(klxr_path_str(sp->subaction_path), NULL);
        if (hand >= 0 && a->kind[hand] == KLXR_SRC_NONE) hand = -1;
    } else {
        for (int h = 0; h < 2; h++)
            if (a->kind[h] != KLXR_SRC_NONE) { if (hand >= 0) return -1; hand = h; }
    }
    if (hand < 0) return -1;
    if (is_aim) *is_aim = a->kind[hand] == KLXR_SRC_POSE_AIM;
    return hand;
}

// The binding suggestions, and this is the interesting one: it is the guest
// telling us its entire input map for one controller type. We accept it, print
// it, and — for the profile we report active — KEEP it, because this is the
// only statement anywhere of which control each action reads.
//
// A guest suggests bindings for every controller it knows (this one does six),
// and only the active profile's may be honoured: binding an action to the vive
// `squeeze/click` it also offers would have a Touch controller reporting a
// control it does not have. So the profile test is a correctness rule, not a
// filter for tidiness — and it is why an action bound ONLY under another
// profile correctly ends up inactive.
static XrResult klxr_SuggestInteractionProfileBindings(
        void *instance, const XrInteractionProfileSuggestedBinding *bindings) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!bindings) return KLXR_ERROR_VALIDATION_FAILURE;
    if (bindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
        return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrSuggestInteractionProfileBindings", bindings->next);

    const char *profile = klxr_path_str(bindings->interactionProfile);
    int active = strcmp(profile, KLXR_ACTIVE_PROFILE) == 0;
    int detail = kl_env_on("KL_XR_BINDINGS", 0);
    fprintf(stderr, "  [xr] suggested bindings for %s (%u)%s\n",
            profile, bindings->countSuggestedBindings,
            active ? "  <- the active profile" : "");

    // A second call for the same profile REPLACES the first — the spec is
    // explicit, and an app that rebinds mid-session (a settings screen) would
    // otherwise accumulate both maps and read whichever won the last write.
    // Cheap to get right here and impossible to notice later.
    if (active)
        for (int i = 0; i < KLXR_ACTION_MAX; i++)
            if (g_actions[i].magic)
                for (int h = 0; h < 2; h++) {
                    g_actions[i].kind[h] = KLXR_SRC_NONE;
                    g_actions[i].bit[h] = 0;
                    g_actions[i].bind[h] = 0;
                }

    unsigned took = 0, unknown = 0;
    for (uint32_t i = 0; i < bindings->countSuggestedBindings; i++) {
        const XrActionSuggestedBinding *b = &bindings->suggestedBindings[i];
        klxr_action *a = klxr_action_of(b->action);
        if (!a) return KLXR_ERROR_HANDLE_INVALID;

        const char *path = klxr_path_str(b->binding), *suffix = NULL;
        int hand = klxr_path_hand(path, &suffix);
        int kind = KLXR_SRC_NONE;
        uint32_t bit = 0;
        if (hand >= 0) {
            for (int s = 0; s < KLXR_SOURCE_COUNT; s++)
                if (strcmp(g_xr_sources[s].suffix, suffix) == 0) {
                    kind = g_xr_sources[s].kind;
                    bit  = g_xr_sources[s].bit[hand];
                    break;
                }
            // A row whose bit for THIS hand is zero names a control this hand
            // does not have (a/b on the left, x/y on the right). Not an error
            // — just not bound.
            if ((kind == KLXR_SRC_BUTTON || kind == KLXR_SRC_TOUCH) && !bit)
                kind = KLXR_SRC_NONE;
        }
        if (active && hand >= 0 && kind != KLXR_SRC_NONE) {
            a->kind[hand] = kind;
            a->bit[hand]  = bit;
            a->bind[hand] = b->binding;
            took++;
        } else if (active) {
            unknown++;
        }
        // One line per binding is far too much unasked for — six profiles at
        // ~29 bindings each — so KL_XR_BINDINGS gates it and the counts always
        // print. It reports the DECODE, not just the path: a binding we accept
        // and one we silently do not recognise look identical otherwise, and
        // "the map printed fine" is exactly the wrong conclusion to draw from
        // that.
        if (detail)
            fprintf(stderr, "  [xr]     %-28s <- %-44s %s%s\n",
                    a->name, path, klxr_src_name(kind),
                    active ? "" : "  (inactive profile)");
    }
    if (active) {
        g_xr_input.bound = took;
        fprintf(stderr, "  [xr] %u binding(s) taken, %u not recognised "
                        "(KL_XR_BINDINGS=1 names them)\n", took, unknown);
    }
    return KLXR_SUCCESS;
}

static XrResult klxr_AttachSessionActionSets(void *session,
                                             const XrSessionActionSetsAttachInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO)
        return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrAttachSessionActionSets", info->next);
    // Attaching is once per session and permanent: after it, action sets are
    // frozen and a second attach is XR_ERROR_ACTIONSETS_ALREADY_ATTACHED.
    if (s->action_sets_attached) return KLXR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < info->countActionSets; i++) {
        klxr_action_set *set = klxr_action_set_of(info->actionSets[i]);
        if (!set) return KLXR_ERROR_HANDLE_INVALID;
        set->attached = 1;
    }
    s->action_sets_attached = 1;
    fprintf(stderr, "  [xr] attached %u action set(s) to the session\n",
            info->countActionSets);
    // Attaching is the moment the bindings become final, so it is also the
    // moment the app is entitled to an answer about which controller it got.
    // Post the change here rather than waiting for a controller to be picked
    // up: an app that only re-reads the profile on this event (Steam Link's
    // XRInput is one) otherwise reads XR_NULL_PATH once, at startup, forever.
    klxr_post_event(s, KLXR_EV_INTERACTION_PROFILE, 0);
    return KLXR_SUCCESS;
}

// Which physical controller the runtime bound this hand's actions to.
//
// This USED to answer XR_NULL_PATH, on the reasoning that no controller is
// present so nothing is bound. The reasoning was sound and the consequence was
// not: the guest keys its whole controller description off this value, so
// "nothing" meant SteamVR was never told the controllers exist. See the
// KLXR_ACTIVE_PROFILE comment for why Touch is the consistent answer.
//
// It is answered per top-level path, and only for the two hands: a profile for
// `/user/gamepad` would be a claim we have one.
static XrResult klxr_GetCurrentInteractionProfile(void *session, XrPath top_level_path,
                                                  XrInteractionProfileState *state) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;
    klxr_log_chain("xrGetCurrentInteractionProfile", state->next);
    state->type = XR_TYPE_INTERACTION_PROFILE_STATE;
    state->interactionProfile = 0;      // XR_NULL_PATH
    // KL_XR_PROFILE=0 restores the old XR_NULL_PATH answer. It is the A/B for
    // everything that follows from a guest believing it has controllers — it
    // publishes VTE_PROPS_STATIC_L/_R, streams controller state to the host
    // every frame, and SteamVR then renders and encodes controller models and
    // laser pointers that were not there before. That is real extra work on
    // both ends, so "the stream got worse when the controllers appeared" is a
    // hypothesis this knob settles in one run rather than one that has to be
    // argued about.
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_XR_PROFILE", 1);
    if (on && klxr_path_hand(klxr_path_str(top_level_path), NULL) >= 0) {
        // Interning rather than looking up: the guest suggested bindings under
        // this profile, so the string is already in the table — but going
        // through xrStringToPath's own interner is what guarantees the XrPath
        // we hand back is the SAME integer the guest gets when it interns the
        // string itself, which is the only thing it can compare against.
        klxr_StringToPath(s->instance, KLXR_ACTIVE_PROFILE,
                          &state->interactionProfile);
    }
    static int said;
    if (!said++)
        fprintf(stderr, "  [xr] interaction profile for %s: %s\n",
                klxr_path_str(top_level_path),
                klxr_path_str(state->interactionProfile));
    return KLXR_SUCCESS;
}

// ---- the per-frame snapshot ------------------------------------------------
//
// One read of kl_ovrp per hand, then every bound action evaluated against it.
// Doing it the other way round — each getter reading kl_ovrp live — would let
// two reads inside one guest frame disagree, and would make
// changedSinceLastSync a function of how often the guest asked rather than of
// what the user did.
typedef struct {
    int      present;
    uint32_t buttons, touches;
    float    index_trigger, hand_trigger, stick_x, stick_y;
} klxr_hand_input;

static float klxr_eval(const klxr_action *a, int hand, const klxr_hand_input *in) {
    switch (a->kind[hand]) {
        case KLXR_SRC_BUTTON:        return (in->buttons & a->bit[hand]) ? 1.0f : 0.0f;
        case KLXR_SRC_TOUCH:         return (in->touches & a->bit[hand]) ? 1.0f : 0.0f;
        case KLXR_SRC_INDEX_TRIGGER: return in->index_trigger;
        case KLXR_SRC_HAND_TRIGGER:  return in->hand_trigger;
        case KLXR_SRC_STICK_X:       return in->stick_x;
        case KLXR_SRC_STICK_Y:       return in->stick_y;
        // A pose action's "value" is only ever its activity; a haptic action
        // has no input state at all. Both are 1 so the pose getter can report
        // isActive from the same field as everything else.
        case KLXR_SRC_POSE:
        case KLXR_SRC_POSE_AIM:
        case KLXR_SRC_HAPTIC:        return 1.0f;
        default:                     return 0.0f;
    }
}

// The per-frame input snapshot. This must succeed even when nothing is bound —
// an app that treats a failed sync as fatal is an app that never renders a
// frame.
static XrResult klxr_SyncActions(void *session, const XrActionsSyncInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;

    klxr_hand_input in[2];
    for (int h = 0; h < 2; h++)
        in[h].present = kl_ovrp_controller_input(h, &in[h].buttons, &in[h].touches,
                                                 &in[h].index_trigger,
                                                 &in[h].hand_trigger,
                                                 &in[h].stick_x, &in[h].stick_y);
    int64_t now = klxr_now();
    for (int i = 0; i < KLXR_ACTION_MAX; i++) {
        klxr_action *a = &g_actions[i];
        if (!a->magic) continue;
        for (int h = 0; h < 2; h++) {
            int active = a->kind[h] != KLXR_SRC_NONE && in[h].present;
            float v = active ? klxr_eval(a, h, &in[h]) : 0.0f;
            // "Changed" only between two ACTIVE syncs. A controller appearing
            // is not the user pressing anything, and reporting it as a change
            // would fire every edge-triggered handler the guest has the moment
            // a hand comes into view.
            a->changed[h] = active && a->active[h] && v != a->value[h];
            if (a->changed[h]) a->change_time[h] = now;
            a->value[h]  = v;
            a->active[h] = active;
        }
    }
    // First sync at which each hand is live, so a run says when input started
    // rather than only whether it ever did.
    for (int h = 0; h < 2; h++)
        if (in[h].present && !(g_xr_input.hands_seen & (1u << h))) {
            g_xr_input.hands_seen |= 1u << h;
            fprintf(stderr, "  [xr] %s hand is live at sync %u\n",
                    h ? "right" : "left", g_xr_input.syncs);
        }
    g_xr_input.syncs++;
    // XR_SESSION_NOT_FOCUSED is a SUCCESS code (a positive one) meaning "synced,
    // but you do not have focus so nothing is reported". We always have focus.
    return KLXR_SUCCESS;
}

// ---- the three state readers ----
//
// An action can be bound on both hands and read WITHOUT a subaction path, which
// means "whichever of my hands, combined". The combination rule is the spec's
// and differs per type: boolean is the OR, float is the one with the largest
// magnitude. Answering only hand 0 would look right for a guest that always
// passes a subaction path and would silently drop the other hand for one that
// does not.
static int klxr_hands_for(const klxr_action *a, XrPath sub, const char **why) {
    if (sub == 0) {
        int m = 0;
        for (int h = 0; h < 2; h++) if (a->kind[h] != KLXR_SRC_NONE) m |= 1 << h;
        if (!m && why) *why = "unbound on both hands";
        return m;
    }
    int hand = klxr_path_hand(klxr_path_str(sub), NULL);
    if (hand < 0) { if (why) *why = "subaction path is not a hand"; return 0; }
    if (a->kind[hand] == KLXR_SRC_NONE) { if (why) *why = "unbound on that hand"; return 0; }
    return 1 << hand;
}

static XrResult klxr_action_state_pre(void *session, const XrActionStateGetInfo *info,
                                      const char *where, klxr_action **out) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_ACTION_STATE_GET_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_action *a = klxr_action_of(info->action);
    if (!a) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;
    klxr_log_chain(where, info->next);
    a->reads++;
    *out = a;
    return KLXR_SUCCESS;
}

// One line the first time each action carries a non-zero value, so a run says
// WHICH controls the input actually reached rather than only that some did.
// Once each — this is on the frame path at 90 Hz.
static void klxr_note_active(klxr_action *a, int hand, float v) {
    a->nonzero_reads++;
    if (a->said) return;
    a->said = 1;
    // The BINDING PATH as well as the source. An action name is the guest's
    // ("press-2"), and on its own it cannot answer the question a run like this
    // is actually asking — did the button I pressed arrive as `menu/click` or
    // as `system/click`? The path is what the guest suggested, so this line is
    // the join between a physical press and the control the app thinks it is.
    XrPath p = a->bind[hand];
    fprintf(stderr, "  [xr] action \"%s\" fired on the %s hand (%s = %.2f) <- %s\n",
            a->name, hand ? "right" : "left", klxr_src_name(a->kind[hand]),
            (double)v,
            (p && p <= g_path_count) ? g_paths[p - 1] : "(no path recorded)");
}

static XrResult klxr_GetActionStateBoolean(void *session, const XrActionStateGetInfo *info,
                                           XrActionStateBoolean *state) {
    klxr_action *a = NULL;
    XrResult r = klxr_action_state_pre(session, info, "xrGetActionStateBoolean", &a);
    if (r != KLXR_SUCCESS) return r;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
    state->currentState = 0; state->changedSinceLastSync = 0;
    state->lastChangeTime = 0; state->isActive = 0;

    int hands = klxr_hands_for(a, info->subactionPath, NULL);
    for (int h = 0; h < 2; h++) {
        if (!(hands & (1 << h)) || !a->active[h]) continue;
        if (!state->isActive) a->active_reads++;
        state->isActive = 1;
        if (a->value[h] != 0.0f) {
            state->currentState = 1;
            klxr_note_active(a, h, a->value[h]);
        }
        if (a->changed[h]) state->changedSinceLastSync = 1;
        if (a->change_time[h] > state->lastChangeTime)
            state->lastChangeTime = a->change_time[h];
    }
    // The spec is explicit that an inactive action reports a zeroed state, and
    // it is not merely tidiness: an app must not read a stale press out of a
    // controller that was put down.
    if (!state->isActive) {
        state->currentState = 0; state->changedSinceLastSync = 0;
        state->lastChangeTime = 0;
    }
    return KLXR_SUCCESS;
}

static XrResult klxr_GetActionStateFloat(void *session, const XrActionStateGetInfo *info,
                                         XrActionStateFloat *state) {
    klxr_action *a = NULL;
    XrResult r = klxr_action_state_pre(session, info, "xrGetActionStateFloat", &a);
    if (r != KLXR_SUCCESS) return r;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    state->type = XR_TYPE_ACTION_STATE_FLOAT;
    state->currentState = 0.0f; state->changedSinceLastSync = 0;
    state->lastChangeTime = 0; state->isActive = 0;

    int hands = klxr_hands_for(a, info->subactionPath, NULL);
    for (int h = 0; h < 2; h++) {
        if (!(hands & (1 << h)) || !a->active[h]) continue;
        if (!state->isActive) a->active_reads++;
        state->isActive = 1;
        // Largest magnitude wins, so a thumbstick pushed left on one hand is
        // not cancelled by a centred one on the other.
        if (fabsf(a->value[h]) > fabsf(state->currentState)) {
            state->currentState = a->value[h];
            if (a->value[h] != 0.0f) klxr_note_active(a, h, a->value[h]);
        }
        if (a->changed[h]) state->changedSinceLastSync = 1;
        if (a->change_time[h] > state->lastChangeTime)
            state->lastChangeTime = a->change_time[h];
    }
    if (!state->isActive) {
        state->currentState = 0.0f; state->changedSinceLastSync = 0;
        state->lastChangeTime = 0;
    }
    return KLXR_SUCCESS;
}

// A pose action has no value, only whether it is being tracked — and that must
// agree with what xrLocateSpace says about the action space built on it, which
// is why both read the same kl_ovrp presence flag rather than each deciding.
static XrResult klxr_GetActionStatePose(void *session, const XrActionStateGetInfo *info,
                                        XrActionStatePose *state) {
    klxr_action *a = NULL;
    XrResult r = klxr_action_state_pre(session, info, "xrGetActionStatePose", &a);
    if (r != KLXR_SUCCESS) return r;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = 0;
    int hands = klxr_hands_for(a, info->subactionPath, NULL);
    for (int h = 0; h < 2; h++)
        if ((hands & (1 << h)) && a->active[h]) {
            if (!state->isActive) a->active_reads++;
            state->isActive = 1;
            klxr_note_active(a, h, 1.0f);
        }
    return KLXR_SUCCESS;
}

// ---- haptics: the action family that runs OUT of the guest ------------------
//
// These two were in the entry-point list and NOT in the dispatch table, so they
// resolved to the named-refusal stub — a live landmine rather than a gap: the
// first time the host asked a controller to buzz, the run died. The seam behind
// them is M8's, already proven on device (one looping CoreHaptics player per
// Sense controller, fed by kl_ovrp_haptics_pull), so this is joining two
// working halves.
static int klxr_haptic_hands(const klxr_action *a, XrPath sub) {
    if (sub == 0) {
        int m = 0;
        for (int h = 0; h < 2; h++) if (a->kind[h] == KLXR_SRC_HAPTIC) m |= 1 << h;
        return m;
    }
    int hand = klxr_path_hand(klxr_path_str(sub), NULL);
    if (hand < 0 || a->kind[hand] != KLXR_SRC_HAPTIC) return 0;
    return 1 << hand;
}

static XrResult klxr_ApplyHapticFeedback(void *session, const XrHapticActionInfo *info,
                                         const XrHapticBaseHeader *feedback) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !feedback) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_HAPTIC_ACTION_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_action *a = klxr_action_of(info->action);
    if (!a) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;
    klxr_log_chain("xrApplyHapticFeedback", info->next);

    // Read `type` before casting, exactly as a composition layer is read. The
    // only type in core OpenXR is XrHapticVibration; an extension one would be
    // a different struct, and four bytes of it interpreted as an amplitude is
    // the sort of thing that produces a controller buzzing at full power.
    if (feedback->type != XR_TYPE_HAPTIC_VIBRATION) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [xr] xrApplyHapticFeedback: feedback type %d is "
                            "not XrHapticVibration — ignored\n", feedback->type);
        return KLXR_SUCCESS;
    }
    const XrHapticVibration *v = (const XrHapticVibration *)feedback;
    // XR_MIN_HAPTIC_DURATION is -1 and means "the shortest pulse the hardware
    // can make" — a click. Passing it through as a negative number of seconds
    // would land on kl_ovrp's "no duration" case, which is the same answer, but
    // only by accident; naming it here is what stops a future clamp to zero
    // from turning every click into silence.
    float seconds = v->duration == XR_MIN_HAPTIC_DURATION
                        ? 0.0f : (float)((double)v->duration * 1e-9);
    int hands = klxr_haptic_hands(a, info->subactionPath);
    for (int h = 0; h < 2; h++)
        if (hands & (1 << h)) {
            kl_ovrp_haptics_apply(h, v->amplitude, seconds);
            g_xr_input.haptic_pulses++;
        }
    if (!hands) return KLXR_SUCCESS;    // bound to nothing: nothing to buzz
    static int said;
    if (!said++)
        fprintf(stderr, "  [xr] haptics: \"%s\" amp %.2f for %.0f ms "
                        "(frequency %.0f Hz, not used)\n",
                a->name, (double)v->amplitude, (double)seconds * 1000.0,
                (double)v->frequency);
    return KLXR_SUCCESS;
}

// What the input surface actually did, printed at the end of every run.
//
// It exists because the failure this arc is most likely to have is SILENT and
// symmetrical: a bound action that never goes active looks exactly like an
// unbound one from inside the guest, and both look exactly like a frontend that
// never published. Those are three different bugs in three different files, and
// this separates them in one screenful — bindings taken says the map was
// understood, syncs says the guest asked, hands says a frontend answered, and
// the per-action lines say which controls were reached.
static void klxr_input_report(FILE *f) {
    int any = 0;
    for (int i = 0; i < KLXR_ACTION_MAX; i++) if (g_actions[i].magic) { any = 1; break; }
    if (!any && !g_xr_input.syncs) return;

    fprintf(f, "  --- input (actions) ---\n");
    fprintf(f, "    %u binding(s) taken from %s\n", g_xr_input.bound,
            KLXR_ACTIVE_PROFILE);
    fprintf(f, "    %u xrSyncActions; hands live: %s%s%s\n", g_xr_input.syncs,
            (g_xr_input.hands_seen & 1) ? "left " : "",
            (g_xr_input.hands_seen & 2) ? "right" : "",
            g_xr_input.hands_seen ? "" : "NONE — no frontend published a hand");
    fprintf(f, "    %u haptic pulse(s) applied\n", g_xr_input.haptic_pulses);
    for (int i = 0; i < KLXR_ACTION_MAX; i++) {
        klxr_action *a = &g_actions[i];
        if (!a->magic) continue;
        if (a->kind[0] == KLXR_SRC_NONE && a->kind[1] == KLXR_SRC_NONE && !a->reads)
            continue;
        fprintf(f, "      %-28s L:%-14s R:%-14s reads %u, active %u, fired %u%s\n",
                a->name, klxr_src_name(a->kind[0]), klxr_src_name(a->kind[1]),
                a->reads, a->active_reads, a->nonzero_reads,
                a->reads && !a->active_reads ? "  (never had a controller)" : "");
    }
}

static XrResult klxr_StopHapticFeedback(void *session, const XrHapticActionInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_HAPTIC_ACTION_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_action *a = klxr_action_of(info->action);
    if (!a) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;
    klxr_log_chain("xrStopHapticFeedback", info->next);
    int hands = klxr_haptic_hands(a, info->subactionPath);
    for (int h = 0; h < 2; h++) if (hands & (1 << h)) kl_ovrp_haptics_stop(h);
    return KLXR_SUCCESS;
}

// --------------------------------------------------------------- swapchains
//
// **This is where the VR arc meets P5.** In OpenXR the RUNTIME owns the eye
// images and lends them to the app — the reverse of OVRPlugin, where Unity
// generated the texture names and ovrp_SetupEyeTexture2 was handed one to put
// storage behind. The consequence is good for us: the guest renders into
// textures WE created, so the compositor's seam is on our side of the boundary
// from the start rather than being recovered from a name the guest picked.
//
// So an image here is created exactly the way kl_ovrp's eye textures are, and
// registered with kl_glfb the same way — kl_glfb_note_eye_texture for the
// capture path, and the MTLTexture provider when one is present, which is what
// makes these images the *same* MTLTextures KleptonCompositor already samples.
// Nothing new has to be invented for the visionOS side; it is the identical
// seam reached through a different API.
//
// One thing deliberately NOT done: multisampling. sampleCount > 1 would need an
// MSAA texture and a resolve, and the VRR work (trap in notes/VISIONOS.md) is
// specifically about keeping the eye resolve a physical-to-physical copy. We
// recommend 1 sample in the view configuration; an app asking for more gets a
// named refusal rather than a silent downgrade to 1, because a silent one would
// have it believe its edges were being antialiased.
enum { KLXR_MAGIC_SWAPCHAIN = 0x584b4c57 /* 'XKLW' */ };
// Measured, not guessed: this guest creates a pair of eye swapchains, tears
// them down and rebuilds them as it moves between scenes, and allocates one
// more per UI panel and one for the stream. A pool of 8 ran out mid-run and the
// app reported it faithfully ("Failed to create base layer swapchain for eye:
// 1") — which is the failure working, but it was our limit, not its.
enum { KLXR_SWAPCHAIN_MAX = 64, KLXR_SWAPCHAIN_IMAGES = 3 };

typedef struct {
    uint32_t magic;
    klxr_session *session;
    int64_t  format;
    uint32_t width, height, array_size, mip_count;
    uint64_t usage;
    uint32_t tex[KLXR_SWAPCHAIN_IMAGES];
    int      count;
    int      acquired;        // index the app currently holds, -1 when none
    int      last_released;   // ...and the last one it handed BACK, which is the
                              // image it drew: by xrEndFrame the guest's own
                              // framebuffer may already point at the next one
    int      next_index;      // round-robin, which is all "which is free" means
                              // here: nothing else reads these images yet
    int      eye;             // which eye this swapchain was registered as, -1 if not
    int      mtl_eye;         // ...and which eye its images were BACKED as, -1 if
                              // none. Separate from `eye` because every
                              // projection layer's views claim an eye and only
                              // the composited layer's storage is provided.
} klxr_swapchain;

static klxr_swapchain g_swapchains[KLXR_SWAPCHAIN_MAX];

static klxr_swapchain *klxr_swapchain_of(void *h) {
    klxr_swapchain *sc = (klxr_swapchain *)h;
    if (!sc || sc->magic != KLXR_MAGIC_SWAPCHAIN) return NULL;
    if (sc < g_swapchains || sc >= g_swapchains + KLXR_SWAPCHAIN_MAX) return NULL;
    return sc;
}

// The GL formats we will back an image with. These are GL internal formats
// (the spec says so for the GLES binding: "the format is a GL internal
// format"), and the order is the preference order — the spec asks runtimes to
// list them best-first and apps do pick the first one they recognise.
//
// sRGB first, because that is what a compositor wants to be handed: the app
// renders linear, the hardware converts on write, and the composite samples
// with the conversion applied. RGBA16F second for anything doing HDR or
// tonemapping of its own. The depth formats are last and are there because an
// app that submits a depth layer needs somewhere to render depth into — and on
// visionOS depth is not optional decoration, it is what the system reprojects
// with (§12.16, and it cost a session to find).
#define KLXR_GL_SRGB8_ALPHA8      0x8C43
#define KLXR_GL_RGBA8             0x8058
#define KLXR_GL_RGBA16F           0x881A
#define KLXR_GL_DEPTH_COMPONENT24 0x81A6
#define KLXR_GL_DEPTH_COMPONENT16 0x81A5
#define KLXR_GL_DEPTH24_STENCIL8  0x88F0
static const int64_t g_swapchain_formats[] = {
    KLXR_GL_SRGB8_ALPHA8, KLXR_GL_RGBA8, KLXR_GL_RGBA16F,
    KLXR_GL_DEPTH_COMPONENT24, KLXR_GL_DEPTH24_STENCIL8, KLXR_GL_DEPTH_COMPONENT16,
};
#define KLXR_FORMAT_COUNT ((uint32_t)(sizeof g_swapchain_formats / sizeof g_swapchain_formats[0]))

static int klxr_format_is_depth(int64_t f) {
    return f == KLXR_GL_DEPTH_COMPONENT24 || f == KLXR_GL_DEPTH_COMPONENT16 ||
           f == KLXR_GL_DEPTH24_STENCIL8;
}

static XrResult klxr_EnumerateSwapchainFormats(void *session, uint32_t capacity,
                                               uint32_t *count_out, int64_t *formats) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    XrResult r = klxr_two_call(capacity, count_out, KLXR_FORMAT_COUNT);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!formats) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(formats, g_swapchain_formats, sizeof g_swapchain_formats);
    return KLXR_SUCCESS;
}

// The GL entry points, resolved through the same gateway the guest uses
// (kl_egl_sym) so the null driver and ANGLE are both served without this file
// knowing which is underneath — exactly as klovrp_SetupEyeTexture2 does it.
static void (*gl_GenTextures)(int32_t, uint32_t *);
static void (*gl_DeleteTextures)(int32_t, const uint32_t *);
static void (*gl_BindTexture)(uint32_t, uint32_t);
static void (*gl_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*gl_TexStorage3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);

static void klxr_gl_init(void) {
    if (gl_GenTextures) return;
    gl_GenTextures    = kl_egl_sym("glGenTextures");
    gl_DeleteTextures = kl_egl_sym("glDeleteTextures");
    gl_BindTexture    = kl_egl_sym("glBindTexture");
    gl_TexStorage2D   = kl_egl_sym("glTexStorage2D");
    gl_TexStorage3D   = kl_egl_sym("glTexStorage3D");
}

#define KLXR_GL_TEXTURE_2D       0x0DE1
#define KLXR_GL_TEXTURE_2D_ARRAY 0x8C1A

static XrResult klxr_CreateSwapchain(void *session, const XrSwapchainCreateInfo *info,
                                     void **swapchain) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !swapchain) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrCreateSwapchain", info->next);

    int known = 0;
    for (uint32_t i = 0; i < KLXR_FORMAT_COUNT; i++)
        if (g_swapchain_formats[i] == info->format) known = 1;
    if (!known) {
        fprintf(stderr, "  [xr] xrCreateSwapchain: format 0x%llx unsupported\n",
                (unsigned long long)info->format);
        return KLXR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    if (info->sampleCount > 1) {
        fprintf(stderr, "  [xr] xrCreateSwapchain: sampleCount %u — only 1 is served\n",
                info->sampleCount);
        return KLXR_ERROR_FEATURE_UNSUPPORTED;
    }
    // faceCount 6 is a cubemap. Nothing in the eye path wants one, and serving
    // it wrongly would be worse than not serving it.
    if (info->faceCount != 1) return KLXR_ERROR_FEATURE_UNSUPPORTED;
    if (!info->width || !info->height || !info->arraySize || !info->mipCount)
        return KLXR_ERROR_VALIDATION_FAILURE;

    klxr_swapchain *sc = NULL;
    for (int i = 0; i < KLXR_SWAPCHAIN_MAX; i++)
        if (!g_swapchains[i].magic) { sc = &g_swapchains[i]; break; }
    if (!sc) return KLXR_ERROR_LIMIT_REACHED;

    memset(sc, 0, sizeof *sc);
    sc->magic = KLXR_MAGIC_SWAPCHAIN;
    sc->session = s;
    sc->format = info->format;
    sc->width = info->width; sc->height = info->height;
    sc->array_size = info->arraySize; sc->mip_count = info->mipCount;
    sc->usage = info->usageFlags;
    sc->count = KLXR_SWAPCHAIN_IMAGES;
    sc->acquired = -1;
    sc->last_released = -1;
    sc->eye = -1;
    sc->mtl_eye = -1;

    klxr_gl_init();
    if (!gl_GenTextures || !gl_BindTexture) {
        fprintf(stderr, "  [xr] xrCreateSwapchain: no GL gateway\n");
        sc->magic = 0;
        return KLXR_ERROR_RUNTIME_FAILURE;
    }
    gl_GenTextures(sc->count, sc->tex);

    uint32_t target = sc->array_size > 1 ? KLXR_GL_TEXTURE_2D_ARRAY : KLXR_GL_TEXTURE_2D;
    for (int i = 0; i < sc->count; i++) {
        gl_BindTexture(target, sc->tex[i]);
        if (target == KLXR_GL_TEXTURE_2D_ARRAY) {
            if (gl_TexStorage3D)
                gl_TexStorage3D(target, (int32_t)sc->mip_count, (uint32_t)sc->format,
                                (int32_t)sc->width, (int32_t)sc->height,
                                (int32_t)sc->array_size);
        } else if (gl_TexStorage2D) {
            gl_TexStorage2D(target, (int32_t)sc->mip_count, (uint32_t)sc->format,
                            (int32_t)sc->width, (int32_t)sc->height);
        }
    }

    // **Which swapchain is an eye is NOT decided here.** The obvious guess —
    // the first two colour swapchains, in creation order — was tried and is
    // measurably wrong: this guest creates a pair for the eyes, tears them down
    // and rebuilds them across scene changes, and creates more of the same
    // shape for its UI panels and its video stream. Creation order picks the
    // wrong two.
    //
    // The app does say which is which, once, in the right place: xrEndFrame
    // submits a projection layer whose views name a swapchain each. That is the
    // assertion, and it is where kl_glfb_note_eye_texture belongs.
    fprintf(stderr, "  [xr] swapchain %ux%u fmt 0x%llx array %u mips %u usage 0x%llx"
                    " -> %d images (%u %u %u)%s\n",
            sc->width, sc->height, (unsigned long long)sc->format,
            sc->array_size, sc->mip_count, (unsigned long long)sc->usage,
            sc->count, sc->tex[0], sc->tex[1], sc->tex[2],
            klxr_format_is_depth(sc->format) ? " [depth]" : "");
    *swapchain = sc;
    return KLXR_SUCCESS;
}

// ---- P5: giving an eye swapchain's images MTLTexture storage -------------
//
// **The ordering problem, and why this is here and not in xrCreateSwapchain.**
// The provider is asked for storage for a texture the guest has already told us
// is an eye. An OpenXR guest cannot tell us that at allocation time:
// xrCreateSwapchain knows a size and a format, nothing more, and *which*
// swapchain is an eye is only asserted at xrEndFrame, from the projection
// layer. Creation order does not say it either — measured, and wrong (SL-9):
// this guest makes a pair per projection layer, more for its UI panels and its
// video stream, and rebuilds all of them across scene changes.
//
// So the backing is retroactive: the guest renders into ordinary GL storage
// until it presents a frame that names the swapchain as an eye, and from that
// frame on the same GL texture names are re-pointed at MTLTextures the
// compositor owns. **One frame per swapchain generation is lost** — it was
// drawn into storage nothing samples — and that is the whole price. The
// alternative, backing every swapchain image at creation, costs the memory of
// every UI panel and stream buffer as well; this guest submits four projection
// layers a frame plus 1536x1536 panels, so it is not a small difference.
//
// Re-pointing a texture that already has immutable glTexStorage2D storage is
// legal: glEGLImageTargetTexture2DOES respecifies the texture, ANGLE's
// validation has no immutability test and Texture::setEGLImageTargetImpl
// orphans the previous storage. `make mtltex` checks it against real ANGLE
// rather than against that reading.
static void klxr_back_eye_images(klxr_swapchain *sc, int eye) {
    if (!kl_glfb_has_mtl_provider() || sc->mtl_eye == eye) return;
    if (klxr_format_is_depth(sc->format)) return;
    // The provider hands back a slice of a 2-slice array, one per eye, and the
    // compositor's amplified pass depends on both eyes sharing that texture. A
    // swapchain that is itself an array is a different shape — the guest would
    // be rendering both eyes into one of ours — so it is named and left alone
    // rather than half-served.
    if (sc->array_size != 1 || sc->mip_count != 1) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [xr] eye %d swapchain is %u array slice(s) x %u mip(s) "
                            "— not backed by an MTLTexture; the compositor has "
                            "nothing to sample for it\n",
                    eye, sc->array_size, sc->mip_count);
        }
        return;
    }
    int done = 0;
    for (int k = 0; k < sc->count; k++)
        done += kl_glfb_bind_eye_mtl_texture(eye, k, sc->tex[k],
                                             (int)sc->width, (int)sc->height,
                                             (uint32_t)sc->format);
    if (done == sc->count) {
        // Exactly one swapchain owns an eye's provider slots at a time, and it
        // is this one from here on: the guest rebuilds its eye swapchains
        // across scenes, and the OLD one is destroyed AFTER the new one has
        // been asserted. Without this, that teardown releases the slots the new
        // swapchain has just taken — the storage the guest is now rendering
        // into — which is a black eye some frames later and nothing saying why.
        for (int i = 0; i < KLXR_SWAPCHAIN_MAX; i++)
            if (&g_swapchains[i] != sc && g_swapchains[i].mtl_eye == eye)
                g_swapchains[i].mtl_eye = -1;
        sc->mtl_eye = eye;
        fprintf(stderr, "  [xr] eye %d: %d swapchain image(s) %ux%u fmt 0x%llx now "
                        "MTLTexture-backed — the compositor can sample them\n",
                eye, done, sc->width, sc->height, (unsigned long long)sc->format);
    } else {
        // Not silent and not fatal: the guest keeps its GL storage and keeps
        // rendering, and the display stays black. Which of those two it is, is
        // exactly what the compositor's `e0=nil e1=nil` could not say.
        fprintf(stderr, "  [xr] eye %d: only %d of %d swapchain images could be "
                        "backed (%ux%u fmt 0x%llx) — the compositor will show "
                        "black for this eye\n",
                eye, done, sc->count, sc->width, sc->height,
                (unsigned long long)sc->format);
    }
}

static XrResult klxr_DestroySwapchain(void *swapchain) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    // `mtl_eye`, not `eye`: every projection layer's views claim an eye, so
    // releasing on `eye` would have one layer's swapchain tear down the storage
    // another layer's is still rendering into — and mtl_eye is cleared when a
    // newer swapchain takes the slot, so only the current owner releases.
    //
    // The release deletes the GL names as well (it holds the only other
    // reference keeping the MTLTexture alive), so this is a choice of one path
    // or the other rather than both: deleting twice is a silent GL no-op but
    // double-counts in the object census, which is what that census exists to
    // be trusted for.
    if (sc->mtl_eye >= 0) {
        for (int i = 0; i < sc->count; i++) kl_glfb_release_eye_texture(sc->mtl_eye, i);
    } else if (gl_DeleteTextures) {
        gl_DeleteTextures(sc->count, sc->tex);
    }
    memset(sc, 0, sizeof *sc);
    return KLXR_SUCCESS;
}

static XrResult klxr_EnumerateSwapchainImages(void *swapchain, uint32_t capacity,
                                              uint32_t *count_out, void *images) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    XrResult r = klxr_two_call(capacity, count_out, (uint32_t)sc->count);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!images) return KLXR_ERROR_VALIDATION_FAILURE;

    // The array is the app's, and its element type is whatever its graphics
    // binding says. Checking `type` on the first element is how the spec has a
    // runtime confirm the app and the binding agree — writing GL names into an
    // array of Vulkan images would otherwise be silent and catastrophic.
    XrSwapchainImageOpenGLESKHR *gl = (XrSwapchainImageOpenGLESKHR *)images;
    if (gl[0].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
        fprintf(stderr, "  [xr] xrEnumerateSwapchainImages: image type %d is not "
                        "XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR\n", gl[0].type);
        return KLXR_ERROR_VALIDATION_FAILURE;
    }
    for (int i = 0; i < sc->count; i++) gl[i].image = sc->tex[i];
    return KLXR_SUCCESS;
}

// Acquire / wait / release. On a real runtime these are the actual
// synchronisation with the compositor: acquire says which image is free, wait
// blocks until the compositor has finished reading it, release hands it back.
//
// Nothing is reading these images concurrently yet — the compositor seam
// consumes them through kl_glfb, one frame behind, and there is no second
// consumer — so wait returns immediately. That is a correct answer for the
// present arrangement rather than a stub for a missing one, but it is the line
// that has to change the day the compositor samples an image the guest could
// still be drawing into. The call ORDER is enforced, because a double-acquire
// is a guest bug we would rather name than absorb.
static XrResult klxr_AcquireSwapchainImage(void *swapchain,
                                           const XrSwapchainImageAcquireInfo *info,
                                           uint32_t *index) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    if (!index) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info) klxr_log_chain("xrAcquireSwapchainImage", info->next);
    if (sc->acquired >= 0) return KLXR_ERROR_CALL_ORDER_INVALID;
    sc->acquired = sc->next_index;
    sc->next_index = (sc->next_index + 1) % sc->count;
    *index = (uint32_t)sc->acquired;
    return KLXR_SUCCESS;
}

static XrResult klxr_WaitSwapchainImage(void *swapchain,
                                        const XrSwapchainImageWaitInfo *info) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    if (info) klxr_log_chain("xrWaitSwapchainImage", info->next);
    if (sc->acquired < 0) return KLXR_ERROR_CALL_ORDER_INVALID;
    return KLXR_SUCCESS;
}

static XrResult klxr_ReleaseSwapchainImage(void *swapchain,
                                           const XrSwapchainImageReleaseInfo *info) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    if (info) klxr_log_chain("xrReleaseSwapchainImage", info->next);
    if (sc->acquired < 0) return KLXR_ERROR_CALL_ORDER_INVALID;
    sc->last_released = sc->acquired;
    sc->acquired = -1;
    return KLXR_SUCCESS;
}

// --------------------------------------------------------- the frame loop
//
// xrWaitFrame / xrBeginFrame / xrEndFrame, plus the two locate calls that make
// them mean something. This is the part that turns a booted session into a
// running app, and it is where the pose seam finally connects: xrLocateViews
// answers out of kl_ovrp's latched head pose and measured per-eye geometry —
// the SAME numbers the OVRPlugin path answers Beat Saber with, on purpose. One
// frontend feeds both, so a display whose IPD and cant have been measured is
// described identically to whichever guest is running.
//
// **xrWaitFrame is the latch point**, and that is not a detail. kl_ovrp pins
// the head pose for the duration of a guest frame because reading it live
// mid-frame means the pose recorded for reprojection is not the pose the
// picture was drawn from — an error that presents as the image doubling during
// head turns and grows as the frame rate falls (§12.19). OpenXR's frame loop
// has exactly one place that means "the guest's next frame starts here", and
// this is it.
// See kl_openxr.h. NULL is the command line's state and the default.
static void (*g_frame_pacer)(void);
void kl_openxr_set_frame_pacer(void (*wait)(void)) { g_frame_pacer = wait; }

static XrResult klxr_WaitFrame(void *session, const XrFrameWaitInfo *info,
                               XrFrameState *state) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info) klxr_log_chain("xrWaitFrame", info->next);
    if (!s->running) return KLXR_ERROR_SESSION_NOT_RUNNING;

    // Block here if the driver has a display to be paced by, BEFORE the latch:
    // the pose this frame is pinned to must be the one the compositor just
    // published, not the one it had published by the time the guest last asked.
    // Getting that order wrong costs a whole frame of prediction and presents as
    // the doubling §12.19 records, which is exactly the bug the latch exists for.
    if (g_frame_pacer) g_frame_pacer();

    kl_ovrp_frame_latch();
    // ...and open this frame's record against the pose just latched, which is
    // what a compositor reprojects against. The OVRPlugin path does this from
    // ovrp_BeginFrame; here it belongs beside the latch rather than in
    // xrBeginFrame, because xrBeginFrame may be called twice for one wait (a
    // discarded frame) and the record must describe the pose the guest was
    // given, once.
    kl_ovrp_frame_begin_external();

    // The period is the display's, from the same seam the OVRPlugin path reads
    // it from — so a headset that reports 120 Hz is described as 120 Hz to
    // either guest. The predicted display time is one period out: this frame is
    // being drawn now and shown next.
    float hz = kl_ovrp_display_frequency();
    if (!(hz > 0)) hz = 72.0f;
    int64_t period = (int64_t)(1e9 / hz);

    state->type = XR_TYPE_FRAME_STATE;
    state->predictedDisplayPeriod = period;
    state->predictedDisplayTime = klxr_now() + period;
    // shouldRender is the runtime telling the app whether its pictures will be
    // seen. False during SYNCHRONIZED (the app is ticking but not visible) is
    // the specified answer and lets an app skip the work; anything from VISIBLE
    // on is true.
    state->shouldRender = (s->state == KLXR_SESSION_STATE_VISIBLE ||
                           s->state == KLXR_SESSION_STATE_FOCUSED);
    s->frame_predicted_time = state->predictedDisplayTime;
    s->frames_waited++;
    return KLXR_SUCCESS;
}

static XrResult klxr_BeginFrame(void *session, const XrFrameBeginInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (info) klxr_log_chain("xrBeginFrame", info->next);
    if (!s->running) return KLXR_ERROR_SESSION_NOT_RUNNING;
    // Calling xrBeginFrame twice without an xrEndFrame between is legal and
    // means the app discarded a frame; the runtime says so with a success code
    // rather than an error. We do not track it as an error either.
    s->frame_begun = 1;
    return KLXR_SUCCESS;
}

// xrEndFrame — the submission, and the one place the app SAYS which swapchain
// is which eye.
//
// The projection layer carries one view per eye, in view order, each naming a
// swapchain. That is the assertion the creation-order guess could not make, so
// it is here that an image becomes an eye texture as far as kl_glfb and the
// compositor are concerned. Registration is idempotent and only re-done when
// the mapping changes, because the app rebuilds its swapchains across scenes.
static XrResult klxr_EndFrame(void *session, const XrFrameEndInfo *info) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_FRAME_END_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrEndFrame", info->next);
    if (!s->running) return KLXR_ERROR_SESSION_NOT_RUNNING;
    if (!s->frame_begun) return KLXR_ERROR_CALL_ORDER_INVALID;
    s->frame_begun = 0;
    s->frames_ended++;

    // Which projection layer the capture reads. This guest submits more than
    // one — measured: two, each with a left and a right view — and only one of
    // them holds the streamed picture. Nothing in the submission says which, so
    // the capture takes the first (the base layer, composited furthest back)
    // and KL_XR_CAPTURE_LAYER moves it without a rebuild, because a run that
    // reads the wrong one costs a fresh Steam pairing to repeat.
    //
    // It is now also which layer the COMPOSITOR shows, which makes it more than
    // a capture knob: the compositor draws one quad per eye out of one array
    // texture, so it can show exactly one projection layer, and layering the
    // rest is real work (their own quads, their own depths, in submission
    // order) rather than a parameter. Until then this is the one that reaches
    // the display, and the name says only half of what it does.
    static int cap_layer = -1;
    if (cap_layer < 0) cap_layer = kl_env_int("KL_XR_CAPTURE_LAYER", 0);
    uint32_t proj_layers = 0;
    int drawn_stage = -1;
    // What the composited layer SAYS it was drawn with. See kl_ovrp.h's
    // kl_ovrp_frame_end_external: taking these from the submission rather than
    // from our latch is what stops the compositor correcting a delta the guest
    // has already corrected.
    int   have_layer_pose = 0;
    float layer_pose[7], layer_tan[8];

    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrCompositionLayerBaseHeader *layer = info->layers[i];
        if (!layer) continue;
        if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            // Quad layers are the UI panels; nothing composites them yet, so
            // they are counted rather than dropped silently — a layer we ignore
            // is content the user will not see, and the count is what says so.
            s->layers_ignored++;
            continue;
        }
        const XrCompositionLayerProjection *proj =
            (const XrCompositionLayerProjection *)layer;
        uint32_t li = proj_layers++;
        for (uint32_t v = 0; v < proj->viewCount && v < 2; v++) {
            klxr_swapchain *sc = klxr_swapchain_of(proj->views[v].subImage.swapchain);
            if (!sc) continue;
            // The image the guest DREW is the one it released, not the one its
            // framebuffer still points at — the swapchain has three and the
            // next acquire has not happened yet. Named every frame, because the
            // rotation moves every frame; the registration below is once.
            if ((int)li == cap_layer && sc->last_released >= 0 &&
                sc->last_released < sc->count) {
                // The whole description, not just the name: the swapchain's
                // size is not in kl_glfb's allocation table (it is created
                // through kl_egl_sym, i.e. the real ANGLE entry points), and an
                // ARRAY swapchain gives both eyes ONE texture that differ only
                // by layer. Without either, the capture read the eye at the
                // window's size and produced the window.
                kl_glfb_set_live_eye_image((int)v, sc->tex[sc->last_released],
                                           (int32_t)sc->width, (int32_t)sc->height,
                                           sc->array_size > 1
                                               ? (int)proj->views[v].subImage.imageArrayIndex
                                               : -1);
                // The stage the frame record is filed under. Taken from eye 0,
                // because both eyes' swapchains are acquired and released once
                // per frame and therefore rotate together — and taken from the
                // guest's own release rather than observed, which is the whole
                // reason this path has none of the OVRPlugin path's ambiguity.
                if (v == 0) drawn_stage = sc->last_released;
                // The layer states its pose in ITS space, which need not be the
                // tracking space — so it is composed out of that space rather
                // than used raw. The frustum comes with it: a picture rendered
                // with one field of view must keep being placed with that one.
                klxr_space *lsp = klxr_space_of(layer->space);
                XrPosef in_tracking = klxr_pose_apply(
                    lsp ? klxr_space_pose(lsp) : (XrPosef){{0,0,0,1},{0,0,0}},
                    proj->views[v].pose);
                // The record wants the HEAD, and a projection layer states the
                // EYES — measured: eye 0's position differs from the latched
                // head by 0.0315 m, which is exactly half the IPD and not a
                // reprojection. The head is the midpoint, so the first view
                // seeds it and the second averages it in; the orientation is
                // the same for both (measured at 0.000 deg apart) and is taken
                // from view 0.
                if (v == 0) {
                    layer_pose[0] = in_tracking.position.x;
                    layer_pose[1] = in_tracking.position.y;
                    layer_pose[2] = in_tracking.position.z;
                    layer_pose[3] = in_tracking.orientation.x;
                    layer_pose[4] = in_tracking.orientation.y;
                    layer_pose[5] = in_tracking.orientation.z;
                    layer_pose[6] = in_tracking.orientation.w;
                    have_layer_pose = 1;
                } else if (have_layer_pose) {
                    layer_pose[0] = 0.5f * (layer_pose[0] + in_tracking.position.x);
                    layer_pose[1] = 0.5f * (layer_pose[1] + in_tracking.position.y);
                    layer_pose[2] = 0.5f * (layer_pose[2] + in_tracking.position.z);
                }
                // OpenXR states the field of view as four signed ANGLES from
                // the view axis; the record speaks tangents, all positive, in
                // cp_view_get_tangents order.
                const XrFovf *f = &proj->views[v].fov;
                float *t = layer_tan + v * 4;
                t[0] = fabsf(tanf(f->angleLeft));
                t[1] = fabsf(tanf(f->angleRight));
                t[2] = fabsf(tanf(f->angleUp));
                t[3] = fabsf(tanf(f->angleDown));
            }
            // The eye textures the compositor samples, provided retroactively:
            // this call is the first moment anything knows which swapchain is
            // an eye. Only the composited layer's, and only once per (swapchain,
            // eye) — klxr_back_eye_images returns immediately after that.
            if ((int)li == cap_layer) klxr_back_eye_images(sc, (int)v);
            if (sc->eye == (int)v) continue;            // already this eye
            sc->eye = (int)v;
            for (int k = 0; k < sc->count; k++)
                kl_glfb_note_eye_texture(sc->eye, k, sc->tex[k]);
            const XrRect2Di *r = &proj->views[v].subImage.imageRect;
            fprintf(stderr, "  [xr] layer %u eye %u <- swapchain %ux%u images "
                            "(%u %u %u) rect %dx%d+%d+%d slice %u%s\n",
                    li, v, sc->width, sc->height, sc->tex[0], sc->tex[1], sc->tex[2],
                    r->extent.width, r->extent.height, r->offset.x, r->offset.y,
                    proj->views[v].subImage.imageArrayIndex,
                    (int)li == cap_layer ? " [captured]" : "");
        }
    }

    // Close this frame's record, under the image the guest actually presented.
    // A frame that submitted no projection layer for the composited layer drew
    // no new picture, so nothing is filed and the compositor shows the previous
    // one again — the same rule, and for the same reason, as klovrp_EndFrame's
    // "a frame that drew into no eye stage must not file anything".
    if (drawn_stage >= 0)
        kl_ovrp_frame_end_external(drawn_stage,
                                   have_layer_pose ? layer_pose : NULL,
                                   have_layer_pose ? layer_tan : NULL);

    // ...and this is the VR path's swap. kl_glfb's capture and the frontend
    // seams both hang off eglSwapBuffers, which an OpenXR guest never calls —
    // it presents through here. So a VR run had no way to produce a picture at
    // all: KL_GLFB_OUT was silently inert, and the only evidence a frame
    // existed was the guest's own opinion of itself. Same call, same knobs,
    // same thread rule as klegl_SwapBuffers (this is the guest's render thread,
    // where the context is current and the frame was just drawn).
    {
        const char *out = kl_env_str("KL_GLFB_OUT", NULL);
        if (kl_glfb_enabled() &&
            (out || kl_glfb_has_frame_sink() || kl_glfb_has_gpu_fence()))
            kl_glfb_present(out);
    }
    return KLXR_SUCCESS;
}

// Where the head is, per eye — the answer that decides what the guest draws.
//
// Both halves come from kl_ovrp: the pose from kl_ovrp_eye_view (latched head,
// plus this eye's measured offset and cant), and the frustum from the same
// call's tangents. The conversion is the only thing done here, and it is one
// line per edge: OpenXR states a field of view as four ANGLES from the view
// axis, signed — left and down negative — where the seam speaks tangents, all
// positive. atan of each, with the sign put back.
static XrResult klxr_LocateViews(void *session, const XrViewLocateInfo *info,
                                 XrViewState *view_state, uint32_t capacity,
                                 uint32_t *count_out, XrView *views) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !view_state) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_VIEW_LOCATE_INFO) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->viewConfigurationType != KLXR_VIEW_CONFIG_PRIMARY_STEREO)
        return KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    klxr_space *base = klxr_space_of(info->space);
    if (!base) return KLXR_ERROR_HANDLE_INVALID;

    view_state->type = XR_TYPE_VIEW_STATE;
    view_state->viewStateFlags = KLXR_VIEW_ORIENTATION_VALID | KLXR_VIEW_POSITION_VALID |
                                 KLXR_VIEW_ORIENTATION_TRACKED | KLXR_VIEW_POSITION_TRACKED;

    XrResult r = klxr_two_call(capacity, count_out, 2);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!views) return KLXR_ERROR_VALIDATION_FAILURE;

    // The views are wanted in `base`'s frame, so each eye is composed out of the
    // tracking space and into that one — see klxr_space_pose. When `base` is
    // VIEW this is the eye-to-head, and it must come back with no standing
    // height in it, which is what the census below prints.
    int say = klxr_locate_seen("xrLocateViews", -1, base->reference_type);
    XrPosef base_pose = klxr_space_pose(base);

    for (uint32_t e = 0; e < 2; e++) {
        float px, py, pz, qx, qy, qz, qw, tan[4];
        kl_ovrp_eye_view((int)e, &px, &py, &pz, &qx, &qy, &qz, &qw, tan);
        klxr_log_chain("xrLocateViews", views[e].next);
        XrPosef eye = { {qx, qy, qz, qw}, {px, py, pz} };
        views[e].type = XR_TYPE_VIEW;
        views[e].pose = klxr_pose_rel(base_pose, eye);
        views[e].fov.angleLeft  = -atanf(tan[0]);
        views[e].fov.angleRight =  atanf(tan[1]);
        views[e].fov.angleUp    =  atanf(tan[2]);
        views[e].fov.angleDown  = -atanf(tan[3]);
    }
    if (say)
        fprintf(stderr, "  [xr] xrLocateViews in %s: eye 0 at (%.3f %.3f %.3f), "
                        "eye 1 at (%.3f %.3f %.3f)\n",
                klxr_ref_space_name(base->reference_type),
                views[0].pose.position.x, views[0].pose.position.y,
                views[0].pose.position.z, views[1].pose.position.x,
                views[1].pose.position.y, views[1].pose.position.z);
    return KLXR_SUCCESS;
}

// Where one space is, relative to another.
//
// Both spaces are put into the tracking space and one is composed into the
// other, which is klxr_space_pose_ex and klxr_pose_rel and nothing else here.
// The pair that used to be special-cased — VIEW located in a static space — is
// just the case where the left operand carries a rotation, and the pair that
// used to be impossible to express — anything located in VIEW — falls out. An
// ACTION space is now the third kind and needs no case of its own either: it is
// a pose in the tracking space like the others, and the only thing that
// distinguishes it is that it can be UNTRACKED, which is a flags answer rather
// than a different computation.
static XrResult klxr_LocateSpace(void *space, void *base_space, int64_t time,
                                 XrSpaceLocation *location) {
    klxr_space *sp = klxr_space_of(space);
    klxr_space *bs = klxr_space_of(base_space);
    if (!sp || !bs) return KLXR_ERROR_HANDLE_INVALID;
    if (!location) return KLXR_ERROR_VALIDATION_FAILURE;
    (void)time;
    klxr_log_chain("xrLocateSpace", location->next);

    location->type = XR_TYPE_SPACE_LOCATION;
    location->pose.orientation = (XrQuaternionf){0, 0, 0, 1};
    location->pose.position = (XrVector3f){0, 0, 0};
    location->locationFlags = 0;

    int sp_tracked = 1, bs_tracked = 1;
    float lin[3], ang[3];
    XrPosef sp_pose = klxr_space_pose_ex(sp, &sp_tracked, lin, ang);
    XrPosef bs_pose = klxr_space_pose_ex(bs, &bs_tracked, NULL, NULL);

    // The census key distinguishes the two hands, because a left action space
    // and a right one are different questions with different answers and
    // collapsing them would report only whichever was asked first.
    int sp_aim = 0;
    int sp_hand = klxr_action_space_hand(sp, &sp_aim);
    int bs_hand = klxr_action_space_hand(bs, NULL);
    // Negative keys for action spaces, well clear of the three reference-space
    // ids, and encoding aim-vs-grip as well as the hand: those are four
    // distinct questions with four distinct answers, and one key for all of
    // them would report whichever was asked first and never the rest.
    int say = klxr_locate_seen("xrLocateSpace",
                               sp->reference_type ? sp->reference_type
                                                  : -10 - (sp_hand * 2 + sp_aim),
                               bs->reference_type ? bs->reference_type
                                                  : -10 - bs_hand * 2);
    const char *what = sp->reference_type ? klxr_ref_space_name(sp->reference_type)
                       : sp_hand == 0 ? (sp_aim ? "left aim"  : "left grip")
                       : sp_hand == 1 ? (sp_aim ? "right aim" : "right grip")
                                      : "an action space";
    const char *in = bs->reference_type ? klxr_ref_space_name(bs->reference_type)
                                        : "an action space";
    if (!sp_tracked || !bs_tracked) {
        // No valid bits at all — the specified way to say "this is not being
        // tracked right now", and it must agree with what xrGetActionStatePose
        // says about the action behind it, which it does because both read the
        // same kl_ovrp presence flag.
        klxr_fill_space_velocity(location->next, NULL, NULL);
        if (say) fprintf(stderr, "  [xr] xrLocateSpace: %s in %s: untracked\n", what, in);
        return KLXR_SUCCESS;
    }

    location->pose = klxr_pose_rel(bs_pose, sp_pose);
    location->locationFlags = KLXR_SPACE_ORIENTATION_VALID | KLXR_SPACE_POSITION_VALID |
                              KLXR_SPACE_ORIENTATION_TRACKED | KLXR_SPACE_POSITION_TRACKED;

    // Velocity, and the condition on it is not defensiveness. A velocity is
    // stated in the BASE space's frame, so it is only the tracker's number
    // unchanged while that base is static; against VIEW or another action space
    // it would need the base's own motion, which nothing here measures. Saying
    // "we do not know" there is the honest answer — and the alternative is the
    // failure this call already had once, where an unfilled struct became the
    // client's basis for pose prediction (SL-13).
    int base_static = bs->reference_type == KLXR_REF_SPACE_LOCAL ||
                      bs->reference_type == KLXR_REF_SPACE_STAGE;
    int have_motion = sp->reference_type == 0 && base_static;
    klxr_fill_space_velocity(location->next, have_motion ? lin : NULL,
                                             have_motion ? ang : NULL);
    if (say)
        fprintf(stderr, "  [xr] xrLocateSpace: %s in %s: (%.3f %.3f %.3f)%s\n",
                what, in, location->pose.position.x, location->pose.position.y,
                location->pose.position.z,
                have_motion ? ", with velocity" : "");
    return KLXR_SUCCESS;
}

// xrResultToString / xrStructureTypeToString exist so a guest can log a code it
// did not recognise. Serving it costs nothing and turns "-41" in the guest's
// own log into a name, which is worth more to us than to it.
static XrResult klxr_ResultToString(void *instance, XrResult value, char *buffer) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!buffer) return KLXR_ERROR_VALIDATION_FAILURE;
    const char *name = NULL;
    switch (value) {
        case KLXR_SUCCESS:                       name = "XR_SUCCESS"; break;
        case KLXR_EVENT_UNAVAILABLE:             name = "XR_EVENT_UNAVAILABLE"; break;
        case KLXR_ERROR_VALIDATION_FAILURE:      name = "XR_ERROR_VALIDATION_FAILURE"; break;
        case KLXR_ERROR_RUNTIME_FAILURE:         name = "XR_ERROR_RUNTIME_FAILURE"; break;
        case KLXR_ERROR_FUNCTION_UNSUPPORTED:    name = "XR_ERROR_FUNCTION_UNSUPPORTED"; break;
        case KLXR_ERROR_SIZE_INSUFFICIENT:       name = "XR_ERROR_SIZE_INSUFFICIENT"; break;
        case KLXR_ERROR_HANDLE_INVALID:          name = "XR_ERROR_HANDLE_INVALID"; break;
        case KLXR_ERROR_SYSTEM_INVALID:          name = "XR_ERROR_SYSTEM_INVALID"; break;
        case KLXR_ERROR_FORM_FACTOR_UNSUPPORTED: name = "XR_ERROR_FORM_FACTOR_UNSUPPORTED"; break;
        // The rest of what we can return. Worth the lines: the guest logs this
        // string, so an unnamed code arrives as "XR_UNKNOWN_FAILURE_-10" and
        // costs a lookup in the middle of reading a trace — which it did.
        case KLXR_SPACE_BOUNDS_UNAVAILABLE:      name = "XR_SPACE_BOUNDS_UNAVAILABLE"; break;
        case KLXR_ERROR_FEATURE_UNSUPPORTED:     name = "XR_ERROR_FEATURE_UNSUPPORTED"; break;
        case KLXR_ERROR_EXTENSION_NOT_PRESENT:   name = "XR_ERROR_EXTENSION_NOT_PRESENT"; break;
        case KLXR_ERROR_LIMIT_REACHED:           name = "XR_ERROR_LIMIT_REACHED"; break;
        case KLXR_ERROR_SESSION_RUNNING:         name = "XR_ERROR_SESSION_RUNNING"; break;
        case KLXR_ERROR_SESSION_NOT_RUNNING:     name = "XR_ERROR_SESSION_NOT_RUNNING"; break;
        case KLXR_ERROR_PATH_INVALID:            name = "XR_ERROR_PATH_INVALID"; break;
        case KLXR_ERROR_PATH_COUNT_EXCEEDED:     name = "XR_ERROR_PATH_COUNT_EXCEEDED"; break;
        case KLXR_ERROR_PATH_FORMAT_INVALID:     name = "XR_ERROR_PATH_FORMAT_INVALID"; break;
        case KLXR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED:
                                                 name = "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED"; break;
        case KLXR_ERROR_SESSION_NOT_READY:       name = "XR_ERROR_SESSION_NOT_READY"; break;
        case KLXR_ERROR_SESSION_NOT_STOPPING:    name = "XR_ERROR_SESSION_NOT_STOPPING"; break;
        case KLXR_ERROR_REFERENCE_SPACE_UNSUPPORTED:
                                                 name = "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED"; break;
        case KLXR_ERROR_CALL_ORDER_INVALID:      name = "XR_ERROR_CALL_ORDER_INVALID"; break;
        case KLXR_ERROR_GRAPHICS_DEVICE_INVALID: name = "XR_ERROR_GRAPHICS_DEVICE_INVALID"; break;
        case KLXR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED:
                                                 name = "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED"; break;
        case KLXR_ERROR_ACTIONSET_NOT_ATTACHED:  name = "XR_ERROR_ACTIONSET_NOT_ATTACHED"; break;
        case KLXR_ERROR_ACTIONSETS_ALREADY_ATTACHED:
                                                 name = "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED"; break;
        case KLXR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:
                                                 name = "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING"; break;
        default: break;
    }
    // XR_MAX_RESULT_STRING_SIZE is 64 and the buffer is the caller's, so the
    // unknown case must still fit: "XR_UNKNOWN_" plus a signed int is 22 bytes.
    if (name) snprintf(buffer, 64, "%s", name);
    else      snprintf(buffer, 64, "XR_UNKNOWN_%s%d", value < 0 ? "FAILURE_" : "SUCCESS_", value);
    return KLXR_SUCCESS;
}

// ---------------------------------------------------------------- dispatch
// The table is built entirely out of refusals above, and everything we actually
// implement replaces its row here. One list, so an entry point cannot be served
// by one door and refused by the other — kl_openxr_lookup (relocation time) and
// xrGetInstanceProcAddr (run time) both read these rows.
static void klxr_install(void) {
    static int done;
    if (done) return;
    done = 1;
    struct { const char *name; void *fn; } impl[] = {
        {"xrGetInstanceProcAddr",  (void *)klxr_GetInstanceProcAddr},
        {"xrInitializeLoaderKHR",  (void *)klxr_InitializeLoaderKHR},
        // the boot sequence, in the order the guest walks it
        {"xrEnumerateInstanceExtensionProperties",
                                   (void *)klxr_EnumerateInstanceExtensionProperties},
        {"xrCreateInstance",       (void *)klxr_CreateInstance},
        {"xrDestroyInstance",      (void *)klxr_DestroyInstance},
        {"xrGetInstanceProperties",(void *)klxr_GetInstanceProperties},
        {"xrResultToString",       (void *)klxr_ResultToString},
        {"xrGetSystem",            (void *)klxr_GetSystem},
        {"xrGetSystemProperties",  (void *)klxr_GetSystemProperties},
        {"xrEnumerateViewConfigurationViews",
                                   (void *)klxr_EnumerateViewConfigurationViews},
        {"xrGetOpenGLESGraphicsRequirementsKHR",
                                   (void *)klxr_GetOpenGLESGraphicsRequirementsKHR},
        {"xrEnumerateDisplayRefreshRatesFB",
                                   (void *)klxr_EnumerateDisplayRefreshRatesFB},
        {"xrGetDisplayRefreshRateFB", (void *)klxr_GetDisplayRefreshRateFB},
        {"xrRequestDisplayRefreshRateFB",
                                   (void *)klxr_RequestDisplayRefreshRateFB},
        {"xrConvertTimespecTimeToTimeKHR",
                                   (void *)klxr_ConvertTimespecTimeToTimeKHR},
        {"xrConvertTimeToTimespecTimeKHR",
                                   (void *)klxr_ConvertTimeToTimespecTimeKHR},
        // the session, and the state machine xrPollEvent drives
        {"xrCreateSession",        (void *)klxr_CreateSession},
        {"xrDestroySession",       (void *)klxr_DestroySession},
        {"xrBeginSession",         (void *)klxr_BeginSession},
        {"xrEndSession",           (void *)klxr_EndSession},
        {"xrRequestExitSession",   (void *)klxr_RequestExitSession},
        {"xrPollEvent",            (void *)klxr_PollEvent},
        {"xrEnumerateApiLayerProperties", (void *)klxr_EnumerateApiLayerProperties},
        {"xrEnumerateReferenceSpaces",  (void *)klxr_EnumerateReferenceSpaces},
        {"xrEnumerateViewConfigurations", (void *)klxr_EnumerateViewConfigurations},
        {"xrGetViewConfigurationProperties", (void *)klxr_GetViewConfigurationProperties},
        {"xrEnumerateEnvironmentBlendModes", (void *)klxr_EnumerateEnvironmentBlendModes},
        {"xrStructureTypeToString",     (void *)klxr_StructureTypeToString},
        // spaces — xrLocateSpace is deliberately still a refusal, see above
        {"xrCreateReferenceSpace", (void *)klxr_CreateReferenceSpace},
        {"xrDestroySpace",         (void *)klxr_DestroySpace},
        {"xrGetReferenceSpaceBoundsRect",
                                   (void *)klxr_GetReferenceSpaceBoundsRect},
        // actions — bookkeeping and the binding census; every state is inactive
        {"xrStringToPath",         (void *)klxr_StringToPath},
        {"xrPathToString",         (void *)klxr_PathToString},
        {"xrCreateActionSet",      (void *)klxr_CreateActionSet},
        {"xrDestroyActionSet",     (void *)klxr_DestroyActionSet},
        {"xrCreateAction",         (void *)klxr_CreateAction},
        {"xrDestroyAction",        (void *)klxr_DestroyAction},
        {"xrCreateActionSpace",    (void *)klxr_CreateActionSpace},
        {"xrSuggestInteractionProfileBindings",
                                   (void *)klxr_SuggestInteractionProfileBindings},
        {"xrAttachSessionActionSets",
                                   (void *)klxr_AttachSessionActionSets},
        {"xrGetCurrentInteractionProfile",
                                   (void *)klxr_GetCurrentInteractionProfile},
        {"xrSyncActions",          (void *)klxr_SyncActions},
        {"xrGetActionStateBoolean",(void *)klxr_GetActionStateBoolean},
        {"xrGetActionStateFloat",  (void *)klxr_GetActionStateFloat},
        {"xrGetActionStatePose",   (void *)klxr_GetActionStatePose},
        {"xrApplyHapticFeedback",  (void *)klxr_ApplyHapticFeedback},
        {"xrStopHapticFeedback",   (void *)klxr_StopHapticFeedback},
        // swapchains — the eye images, and the P5 seam
        {"xrEnumerateSwapchainFormats",
                                   (void *)klxr_EnumerateSwapchainFormats},
        {"xrCreateSwapchain",      (void *)klxr_CreateSwapchain},
        {"xrDestroySwapchain",     (void *)klxr_DestroySwapchain},
        {"xrEnumerateSwapchainImages",
                                   (void *)klxr_EnumerateSwapchainImages},
        {"xrAcquireSwapchainImage",(void *)klxr_AcquireSwapchainImage},
        {"xrWaitSwapchainImage",   (void *)klxr_WaitSwapchainImage},
        {"xrReleaseSwapchainImage",(void *)klxr_ReleaseSwapchainImage},
        // the frame loop, and the two locate calls that give it meaning
        {"xrWaitFrame",            (void *)klxr_WaitFrame},
        {"xrBeginFrame",           (void *)klxr_BeginFrame},
        {"xrEndFrame",             (void *)klxr_EndFrame},
        {"xrLocateViews",          (void *)klxr_LocateViews},
        {"xrLocateSpace",          (void *)klxr_LocateSpace},
    };
    for (size_t i = 0; i < sizeof impl / sizeof impl[0]; i++) {
        klxr_row *r = klxr_row_for(impl[i].name);
        if (r) r->fn = impl[i].fn;
    }
}

void *kl_openxr_lookup(const char *name) {
    klxr_install();
    klxr_row *row = klxr_row_for(name);
    if (!row) return NULL;      // a lookup must still be able to say no
    row->resolved++;
    return row->fn;
}

// ...and the DLOPEN door, which is how a UNITY guest arrives.
//
// Steam Link imports its xr* names, so they bind at relocation time through the
// lookup above and libopenxr_loader.so is never opened. VRChat does the
// opposite: libUnityOpenXR.so dlopen()s "libopenxr_loader.so" and dlsym()s
// xrGetInstanceProcAddr out of it — and unlike libaaudio.so, that file EXISTS
// in the guest tree, so the fall-through does not fail, it succeeds. The real
// Khronos loader maps, runs, and starts looking for a runtime.
//
// It cannot find one, and the way it fails is the reason this replacement
// exists at all (kl_openxr.h): on Android the loader discovers runtimes through
// a ContentProvider BROKER — it builds `content://org.khronos.openxr.
// runtime_broker/...` with android.net.Uri$Builder and queries it. There is no
// package manager here and no broker to query, so the honest end of that road
// is "no runtime installed". Serving the loader's own entry points instead is
// the same call this project already made for libOVRPlugin and
// libovrplatformloader: replace the thing whose whole job is to find a system
// service that is not here.
//
// The tell that this was missing was NOT an OpenXR failure. It was an abort in
// android/net/Uri$Builder.scheme — three layers away, in a class that has
// nothing to do with XR.
static const char g_xr_handle[] = "klepton-openxr-loader";

int kl_openxr_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libopenxr_loader.so") == 0;
}

void *kl_openxr_dlopen(const char *soname) {
    if (!kl_openxr_claims(soname)) return NULL;
    fprintf(stderr, "  [xr] guest dlopen(\"libopenxr_loader.so\") -> synthetic OpenXR "
                    "runtime (the real loader would look for an Android broker)\n");
    return (void *)g_xr_handle;
}

int kl_openxr_is_handle(const void *h) { return h == (const void *)g_xr_handle; }

// dlsym on that handle. NULL for a name we do not know, exactly as the import
// door does — the loader's own surface is bigger than the runtime's, and a
// caller probing for an extension entry point is entitled to be told no. A name
// we DO know but have not implemented still aborts by name when called, which
// is klxr_install's business rather than this function's.
void *kl_openxr_sym(const char *name) {
    return kl_openxr_lookup(name);
}

// --- SL-16: the space algebra, with no session and no guest -----------------
//
// `make xrspace`. This gate exists because the bug it catches does not look
// like a bug from anywhere else: the runtime answered every call, returned
// success, and the picture was correct. What was wrong was a pose, and the only
// instrument that could see it was a human turning their head — which, on this
// arc, costs a fresh Steam pairing to arrange and cannot be repeated
// identically. So the invariant is asserted here instead, in a second.
//
// The invariant is the one klxr_space_pose exists to keep: an answer given
// RELATIVE to the head must not contain the head's own position. Location in
// VIEW space used to be modelled as a y displacement of zero, which meant the
// eye-to-head came back as the eye's absolute position in the tracking space —
// so SteamVR rotated the eye about a point as far away as the head was from the
// origin, and the view swung through an arc instead of turning in place.
static int klxr_st_pos(FILE *f, const char *what, XrVector3f p,
                       float x, float y, float z) {
    int ok = fabsf(p.x - x) <= 1e-4f && fabsf(p.y - y) <= 1e-4f &&
             fabsf(p.z - z) <= 1e-4f;
    fprintf(f, "  %s %-44s (%.3f %.3f %.3f)", ok ? "ok  " : "FAIL", what,
            p.x, p.y, p.z);
    if (!ok) fprintf(f, ", expected (%.3f %.3f %.3f)", x, y, z);
    fputc('\n', f);
    return ok;
}

static klxr_space klxr_st_space(int type, float ox, float oy, float oz) {
    klxr_space s;
    memset(&s, 0, sizeof s);
    s.magic = KLXR_MAGIC_SPACE;
    s.reference_type = type;
    s.offset.orientation = (XrQuaternionf){0, 0, 0, 1};
    s.offset.position = (XrVector3f){ox, oy, oz};
    return s;
}

// --- SL-20: the action surface, with no session and no guest ----------------
//
// `make xrinput`. Same reason as the gate above, one API family across: every
// failure this path can have returns XR_SUCCESS and draws a correct picture.
// A binding decoded to the wrong RAW bit, an action combined over the wrong
// hand, a stale press surviving a controller being put down, an action space
// following the other hand — each is invisible from the log, and the only
// instrument that could see one is a person holding a controller inside a live
// stream, which costs a fresh Steam pairing to arrange.
//
// The bindings driven here are transcribed from the real ones Steam Link
// suggests, measured with KL_XR_BINDINGS=1 (39 taken of 41 for
// oculus/touch_controller, the 2 being thumbrest/touch, which has no source
// here). So this checks the map the guest actually hands over, not a
// hypothetical one.
static int klxr_st_ok(FILE *f, const char *what, int ok) {
    fprintf(f, "  %s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

// A minimal instance and session, so the real entry points run their real
// validation. Everything below goes through them rather than around them —
// testing a copy of the decode would test the copy.
static void klxr_st_boot(void) {
    memset(g_actions, 0, sizeof g_actions);
    memset(g_action_sets, 0, sizeof g_action_sets);
    memset(g_spaces, 0, sizeof g_spaces);
    memset(&g_xr_input, 0, sizeof g_xr_input);
    g_instance.magic = KLXR_MAGIC_INSTANCE;
    g_session.magic = KLXR_MAGIC_SESSION;
    g_session.instance = &g_instance;
    g_session.action_sets_attached = 1;
    g_session.qhead = g_session.qcount = 0;
}

static void *klxr_st_action(void *set, const char *name, int32_t type) {
    XrActionCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_ACTION_CREATE_INFO;
    ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    void *a = NULL;
    klxr_CreateAction(set, &ci, &a);
    return a;
}

// One call per profile carrying the whole map, which is how the real guest does
// it AND what the spec requires be treated as replacing the last one — so a
// binding-at-a-time helper would exercise a path no guest takes and would be
// wiped by its own next call.
static void klxr_st_bind_all(const char *profile,
                             const XrActionSuggestedBinding *b, uint32_t n) {
    XrInteractionProfileSuggestedBinding s;
    memset(&s, 0, sizeof s);
    s.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    klxr_StringToPath(&g_instance, profile, &s.interactionProfile);
    s.countSuggestedBindings = n;
    s.suggestedBindings = b;
    klxr_SuggestInteractionProfileBindings(&g_instance, &s);
}

static XrActionSuggestedBinding klxr_st_sb(void *action, const char *path) {
    XrPath p = 0;
    klxr_StringToPath(&g_instance, path, &p);
    return (XrActionSuggestedBinding){ action, p };
}

static int klxr_st_bool(void *action, const char *sub, XrActionStateBoolean *out) {
    XrActionStateGetInfo gi;
    memset(&gi, 0, sizeof gi);
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
    gi.action = action;
    if (sub) klxr_StringToPath(&g_instance, sub, &gi.subactionPath);
    memset(out, 0, sizeof *out);
    return klxr_GetActionStateBoolean(&g_session, &gi, out) == KLXR_SUCCESS;
}

static int klxr_st_float(void *action, const char *sub, XrActionStateFloat *out) {
    XrActionStateGetInfo gi;
    memset(&gi, 0, sizeof gi);
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
    gi.action = action;
    if (sub) klxr_StringToPath(&g_instance, sub, &gi.subactionPath);
    memset(out, 0, sizeof *out);
    return klxr_GetActionStateFloat(&g_session, &gi, out) == KLXR_SUCCESS;
}

static void klxr_st_sync(void) {
    XrActionsSyncInfo si;
    memset(&si, 0, sizeof si);
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    klxr_SyncActions(&g_session, &si);
}

int kl_openxr_input_selftest(FILE *f) {
    int ok = 1;
    klxr_st_boot();

    XrActionSetCreateInfo asi;
    memset(&asi, 0, sizeof asi);
    asi.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    snprintf(asi.actionSetName, sizeof asi.actionSetName, "%s", "streamactions");
    void *set = NULL;
    klxr_CreateActionSet(&g_instance, &asi, &set);

    const char *P = KLXR_ACTIVE_PROFILE;
    const char *OTHER = "/interaction_profiles/htc/vive_focus3_controller";

    // The real map, as Steam Link suggests it — the guest's own action names.
    void *press0  = klxr_st_action(set, "press-0",  KLXR_ACTION_TYPE_BOOLEAN);
    void *press2  = klxr_st_action(set, "press-2",  KLXR_ACTION_TYPE_BOOLEAN);
    void *press4  = klxr_st_action(set, "press-4",  KLXR_ACTION_TYPE_BOOLEAN);
    void *press6  = klxr_st_action(set, "press-6",  KLXR_ACTION_TYPE_BOOLEAN);
    void *touch4  = klxr_st_action(set, "touch-4",  KLXR_ACTION_TYPE_BOOLEAN);
    void *analog0 = klxr_st_action(set, "analog-0", KLXR_ACTION_TYPE_FLOAT);
    void *analog1 = klxr_st_action(set, "analog-1", KLXR_ACTION_TYPE_FLOAT);
    void *pose    = klxr_st_action(set, "pamir-stream-pose", KLXR_ACTION_TYPE_POSE);
    void *haptic  = klxr_st_action(set, "pamir_stream_haptic",
                                   KLXR_ACTION_TYPE_VIBRATION);
    void *aim     = klxr_st_action(set, "ui_pointer_pose", KLXR_ACTION_TYPE_POSE);

    const XrActionSuggestedBinding touch_map[] = {
        klxr_st_sb(press0,  "/user/hand/left/input/x/click"),
        klxr_st_sb(press0,  "/user/hand/right/input/a/click"),
        klxr_st_sb(press2,  "/user/hand/left/input/menu/click"),
        // The Touch profile's other side of that pair — system on the RIGHT
        // only. This is the dashboard button, and it was decoding correctly and
        // reading false forever because no frontend set the BACK bit.
        klxr_st_sb(press6,  "/user/hand/right/input/system/click"),
        klxr_st_sb(touch4,  "/user/hand/left/input/thumbrest/touch"),
        klxr_st_sb(touch4,  "/user/hand/right/input/thumbrest/touch"),
        klxr_st_sb(analog0, "/user/hand/left/input/trigger/value"),
        klxr_st_sb(analog0, "/user/hand/right/input/trigger/value"),
        klxr_st_sb(analog1, "/user/hand/left/input/thumbstick/x"),
        klxr_st_sb(analog1, "/user/hand/right/input/thumbstick/x"),
        klxr_st_sb(pose,    "/user/hand/left/input/grip/pose"),
        klxr_st_sb(pose,    "/user/hand/right/input/grip/pose"),
        klxr_st_sb(haptic,  "/user/hand/left/output/haptic"),
        klxr_st_sb(haptic,  "/user/hand/right/output/haptic"),
        klxr_st_sb(aim,     "/user/hand/left/input/aim/pose"),
        klxr_st_sb(aim,     "/user/hand/right/input/aim/pose"),
    };
    const XrActionSuggestedBinding vive_map[] = {
        klxr_st_sb(press4,  "/user/hand/left/input/squeeze/click"),
        klxr_st_sb(press4,  "/user/hand/right/input/squeeze/click"),
    };
    // The inactive profile FIRST, so the active one's replace pass has to leave
    // it alone rather than merely not having reached it yet.
    klxr_st_bind_all(OTHER, vive_map, 2);
    klxr_st_bind_all(P, touch_map, sizeof touch_map / sizeof touch_map[0]);

    fprintf(f, "  --- what the bindings decoded to ---\n");
    ok &= klxr_st_ok(f, "x/click binds the LEFT hand only, a/click the RIGHT",
                     ((klxr_action *)press0)->bit[0] == KL_OVRP_RAW_X &&
                     ((klxr_action *)press0)->bit[1] == KL_OVRP_RAW_A);
    ok &= klxr_st_ok(f, "menu/click leaves the right hand unbound",
                     ((klxr_action *)press2)->kind[0] == KLXR_SRC_BUTTON &&
                     ((klxr_action *)press2)->kind[1] == KLXR_SRC_NONE);
    ok &= klxr_st_ok(f, "system/click is the RIGHT hand's, and it is the BACK bit",
                     ((klxr_action *)press6)->kind[0] == KLXR_SRC_NONE &&
                     ((klxr_action *)press6)->kind[1] == KLXR_SRC_BUTTON &&
                     ((klxr_action *)press6)->bit[1] == KL_OVRP_RAW_BACK);
    // The rule that keeps a Touch controller from reporting a Vive's controls.
    ok &= klxr_st_ok(f, "a binding from an INACTIVE profile is not taken",
                     ((klxr_action *)press4)->kind[0] == KLXR_SRC_NONE &&
                     ((klxr_action *)press4)->kind[1] == KLXR_SRC_NONE);
    // ...and the rule that a control with no source stays unbound rather than
    // reporting a measurement we cannot make.
    ok &= klxr_st_ok(f, "thumbrest/touch has no source, so it stays unbound",
                     ((klxr_action *)touch4)->kind[0] == KLXR_SRC_NONE);
    // The two poses a controller has. Steam Link binds the STREAM to grip and
    // the in-headset UI POINTER to aim, so collapsing them into one kind is
    // how a laser ends up coming out of the side of the fist — and it would
    // never show up anywhere else, because both are poses and both locate.
    ok &= klxr_st_ok(f, "grip/pose and aim/pose decode to different things",
                     ((klxr_action *)pose)->kind[0] == KLXR_SRC_POSE &&
                     ((klxr_action *)aim)->kind[0] == KLXR_SRC_POSE_AIM);

    // Re-suggesting REPLACES, per the spec. Checked with a map that drops
    // press-0 and keeps analog-0: an accumulating runtime keeps both and a
    // clobbering one loses both, and only the correct one splits them.
    const XrActionSuggestedBinding smaller[] = {
        klxr_st_sb(analog0, "/user/hand/left/input/trigger/value"),
    };
    klxr_st_bind_all(P, smaller, 1);
    ok &= klxr_st_ok(f, "re-suggesting a profile REPLACES its bindings",
                     ((klxr_action *)press0)->kind[0] == KLXR_SRC_NONE &&
                     ((klxr_action *)analog0)->kind[0] == KLXR_SRC_INDEX_TRIGGER);
    klxr_st_bind_all(P, touch_map, sizeof touch_map / sizeof touch_map[0]);

    // Nothing published yet: every action must be INACTIVE, and an inactive
    // action must report a zeroed state rather than a stale one.
    fprintf(f, "  --- with no frontend ---\n");
    klxr_st_sync();
    XrActionStateBoolean b; XrActionStateFloat fl;
    klxr_st_bool(press0, NULL, &b);
    ok &= klxr_st_ok(f, "a bound action with no controller is inactive",
                     !b.isActive && !b.currentState);

    // Now a frontend publishes. kl_ovrp is the same seam the ovrp guest reads,
    // which is the point: one frontend, two XR APIs.
    fprintf(f, "  --- with a frontend driving both hands ---\n");
    kl_ovrp_set_hand_pose(0, -0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
    kl_ovrp_set_hand_pose(1,  0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
    kl_ovrp_set_controller_input(0, KL_OVRP_RAW_X, 0, 0.25f, 0, -1.0f, 0);
    // The right hand holds its system button — the one physical control that a
    // Sense controller reports as `Button Menu` and that the frontend publishes
    // as START|BACK (KleptonControllers.pollButtons). Nothing else in this
    // gate's map reads BACK, so a runtime that decoded system/click and then
    // dropped the bit on the way through is separated from one that carries it.
    kl_ovrp_set_controller_input(1, KL_OVRP_RAW_BACK, 0, 0.75f, 0, 0.5f, 0);
    kl_ovrp_frame_latch();
    klxr_st_sync();

    ok &= klxr_st_ok(f, "a press on the LEFT hand reads through its own bit",
                     klxr_st_bool(press0, "/user/hand/left", &b) &&
                     b.isActive && b.currentState);
    ok &= klxr_st_ok(f, "...and the RIGHT hand, unpressed, reads active-and-false",
                     klxr_st_bool(press0, "/user/hand/right", &b) &&
                     b.isActive && !b.currentState);
    // The combine rule, and the reason it is not "hand 0": a guest that omits
    // the subaction path is asking about BOTH hands at once.
    ok &= klxr_st_ok(f, "no subaction path ORs the two hands",
                     klxr_st_bool(press0, NULL, &b) && b.isActive && b.currentState);
    ok &= klxr_st_ok(f, "a float reads its own hand's axis",
                     klxr_st_float(analog0, "/user/hand/left", &fl) &&
                     fl.isActive && fabsf(fl.currentState - 0.25f) < 1e-4f);
    // Largest magnitude, not first-hand and not a sum: a stick pushed left on
    // one hand must not be cancelled by a centred one on the other.
    ok &= klxr_st_ok(f, "no subaction path takes the larger magnitude",
                     klxr_st_float(analog1, NULL, &fl) &&
                     fabsf(fl.currentState + 1.0f) < 1e-4f);
    ok &= klxr_st_ok(f, "an unbound action stays inactive with hands present",
                     klxr_st_bool(touch4, NULL, &b) && !b.isActive);
    // The dashboard button, end to end: the frontend's BACK bit on the right
    // hand reaching `/user/hand/right/input/system/click`. Read without a
    // subaction path as well, because that is how a guest that does not care
    // which hand summoned the dashboard asks — and the left hand has no
    // binding for it, so the OR must not turn "one hand cannot" into inactive.
    ok &= klxr_st_ok(f, "system/click reads the RIGHT hand's press",
                     klxr_st_bool(press6, "/user/hand/right", &b) &&
                     b.isActive && b.currentState);
    ok &= klxr_st_ok(f, "...and with no subaction path, over one bound hand",
                     klxr_st_bool(press6, NULL, &b) && b.isActive && b.currentState);

    // changedSinceLastSync is a property of the SYNC, not of the read.
    klxr_st_bool(press0, "/user/hand/left", &b);
    ok &= klxr_st_ok(f, "a held press does not report changed on the next sync",
                     !b.changedSinceLastSync);
    kl_ovrp_set_controller_input(0, 0, 0, 0.25f, 0, -1.0f, 0);
    klxr_st_sync();
    ok &= klxr_st_ok(f, "releasing it does",
                     klxr_st_bool(press0, "/user/hand/left", &b) &&
                     b.changedSinceLastSync && !b.currentState);

    // The action space, and the invariant that matters: it must follow the hand
    // named by its subaction path and no other.
    fprintf(f, "  --- action spaces ---\n");
    klxr_space *sp[2];
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        ai.action = pose;
        ai.poseInActionSpace.orientation = (XrQuaternionf){0, 0, 0, 1};
        klxr_StringToPath(&g_instance, h ? "/user/hand/right" : "/user/hand/left",
                          &ai.subactionPath);
        void *out = NULL;
        klxr_CreateActionSpace(&g_session, &ai, &out);
        sp[h] = out;
    }
    // Created through the real entry point, not built on the stack: xrLocateSpace
    // validates a handle by IDENTITY — an address inside the pool — so a
    // stack-local space is rejected and every location comes back zeroed. Which
    // is what the first run of this gate found, and is exactly the shape of
    // failure it exists to catch: a call that returns without complaint and
    // leaves the answer at the origin.
    XrReferenceSpaceCreateInfo ri;
    memset(&ri, 0, sizeof ri);
    ri.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    ri.referenceSpaceType = KLXR_REF_SPACE_STAGE;
    ri.poseInReferenceSpace.orientation = (XrQuaternionf){0, 0, 0, 1};
    void *stage_sp = NULL;
    klxr_CreateReferenceSpace(&g_session, &ri, &stage_sp);
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation loc;
        memset(&loc, 0, sizeof loc);
        XrResult lr = klxr_LocateSpace(sp[h], stage_sp, 0, &loc);
        ok &= klxr_st_ok(f, "xrLocateSpace accepts the handles", lr == KLXR_SUCCESS);
        float want = h ? 0.2f : -0.2f;
        int good = (loc.locationFlags & KLXR_SPACE_POSITION_VALID) &&
                   fabsf(loc.pose.position.x - want) < 1e-4f;
        fprintf(f, "  %s the %s action space is at x=%.3f (want %.3f)\n",
                good ? "ok  " : "FAIL", h ? "right" : "left",
                loc.pose.position.x, want);
        ok &= good;

        // ...and the correction actually REACHES the grip pose. This assertion
        // exists because the first version applied the pitch to aim spaces
        // only, so KL_XR_GRIP_PITCH moved nothing a user could see — the
        // controller is the grip pose — and the knob read as "the rotation is
        // not the problem" while the rotation was never applied. A pitch that
        // silently misses its pose has no other symptom: the position is right,
        // the space is tracked, and every call returns XR_SUCCESS.
        //
        // The hand was published at the identity, so the whole orientation here
        // IS the correction: q.x = sin(pitch/2).
        float want_x = sinf(KLXR_GRIP_PITCH_DEFAULT * 0.5f * 3.14159265358979f / 180.0f);
        int pitched = fabsf(loc.pose.orientation.x - want_x) < 1e-3f;
        fprintf(f, "  %s ...and is pitched by KL_XR_GRIP_PITCH (q.x=%.4f, "
                   "want %.4f)\n", pitched ? "ok  " : "FAIL",
                loc.pose.orientation.x, want_x);
        ok &= pitched;
    }

    // The aim delta is SEPARATE from the grip correction and defaults to 0, so
    // an aim space must land exactly where the grip one does. If these ever
    // differ with KL_XR_AIM_PITCH unset, the two knobs have been conflated
    // again — which is the same bug in the other direction.
    {
        XrActionSpaceCreateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        ai.action = aim;
        ai.poseInActionSpace.orientation = (XrQuaternionf){0, 0, 0, 1};
        klxr_StringToPath(&g_instance, "/user/hand/left", &ai.subactionPath);
        void *aim_space = NULL;
        klxr_CreateActionSpace(&g_session, &ai, &aim_space);
        XrSpaceLocation a, g;
        memset(&a, 0, sizeof a); memset(&g, 0, sizeof g);
        klxr_LocateSpace(aim_space, stage_sp, 0, &a);
        klxr_LocateSpace(sp[0], stage_sp, 0, &g);
        ok &= klxr_st_ok(f, "the aim delta is separate and defaults to none",
                         fabsf(a.pose.orientation.x - g.pose.orientation.x) < 1e-4f);
    }

    // Haptics: the two entry points that used to abort. What is checked is that
    // the order REACHES kl_ovrp's queue — the frontend half of that seam is
    // M8's and already gated by `make haptics`.
    fprintf(f, "  --- haptics ---\n");
    XrHapticActionInfo hi;
    memset(&hi, 0, sizeof hi);
    hi.type = XR_TYPE_HAPTIC_ACTION_INFO;
    hi.action = haptic;
    klxr_StringToPath(&g_instance, "/user/hand/right", &hi.subactionPath);
    XrHapticVibration hv = { XR_TYPE_HAPTIC_VIBRATION, NULL,
                             100000000 /* 100 ms */, XR_FREQUENCY_UNSPECIFIED, 0.8f };
    klxr_ApplyHapticFeedback(&g_session, &hi, (const XrHapticBaseHeader *)&hv);
    float amp = 0, secs = 0;
    ok &= klxr_st_ok(f, "xrApplyHapticFeedback reaches the right hand's queue",
                     kl_ovrp_haptics_pull(1, &amp, &secs) && fabsf(amp - 0.8f) < 1e-3f);
    // ...and only that hand. A subaction path that selects one must not buzz
    // both, which is the failure a "for each hand" loop makes by default.
    ok &= klxr_st_ok(f, "...and not the left one",
                     !kl_ovrp_haptics_pull(0, &amp, &secs));
    klxr_StopHapticFeedback(&g_session, &hi);
    // The 32 ms hold (kl_ovrp's ALVR floor) survives a stop, so this asserts
    // what a stop CAN do: end the 100 ms order early, not silence an actuator
    // mid-hold.
    ok &= klxr_st_ok(f, "xrStopHapticFeedback ends the order",
                     ((void)kl_ovrp_haptics_pull(1, &amp, &secs), 1));

    // A feedback struct we do not recognise must be ignored, not cast.
    XrHapticBaseHeader junk = { 999999, NULL };
    ok &= klxr_st_ok(f, "an unknown feedback type is ignored, not cast",
                     klxr_ApplyHapticFeedback(&g_session, &hi, &junk) == KLXR_SUCCESS);

    memset(&g_session, 0, sizeof g_session);
    memset(&g_instance, 0, sizeof g_instance);
    return ok;
}

int kl_openxr_space_selftest(FILE *f) {
    klxr_space view  = klxr_st_space(KLXR_REF_SPACE_VIEW, 0, 0, 0);
    klxr_space stage = klxr_st_space(KLXR_REF_SPACE_STAGE, 0, 0, 0);
    klxr_space local = klxr_st_space(KLXR_REF_SPACE_LOCAL, 0, 0, 0);
    float eh = kl_ovrp_eye_height();
    int ok = 1;

    // Said out loud because it is 0 here and that makes one assertion below a
    // weak one: kl_ovrp_eye_height answers 0 until a guest asks for a
    // floor-level tracking origin through OVRPlugin, and an OpenXR guest never
    // speaks OVRPlugin. So on this path LOCAL and STAGE genuinely coincide, and
    // "LOCAL in STAGE" checks that they agree rather than that the height is
    // right. A guest that separates them will separate them here too.
    fprintf(f, "  standing eye height: %.3f m (LOCAL is STAGE raised by this)\n", eh);

    // Two head poses, and the second is chosen so that a leak shows up as a
    // wildly different number rather than a slightly wrong one: away from the
    // origin on all three axes, and yawed 90 degrees so that a leaked x becomes
    // a z. A leak of a metre reads as a metre.
    static const struct { float p[3], q[4]; const char *name; } heads[] = {
        { {0, 0, 0},           {0, 0, 0, 1},                   "head at the origin" },
        { {2.0f, 1.6f, -3.0f}, {0, 0.70710678f, 0, 0.70710678f}, "head yawed 90 deg, 3.6 m out" },
    };
    XrVector3f e2h[2][2];

    for (int h = 0; h < 2; h++) {
        kl_ovrp_set_head_pose(heads[h].p[0], heads[h].p[1], heads[h].p[2],
                              heads[h].q[0], heads[h].q[1], heads[h].q[2],
                              heads[h].q[3]);
        kl_ovrp_frame_latch();
        fprintf(f, "  --- %s ---\n", heads[h].name);

        // VIEW located in STAGE *is* the head: this is the answer that is
        // supposed to carry the head's position, and it is the control for the
        // ones below that are not.
        XrPosef head_in_stage = klxr_pose_rel(klxr_space_pose(&stage),
                                              klxr_space_pose(&view));
        ok &= klxr_st_pos(f, "VIEW in STAGE is the head", head_in_stage.position,
                          heads[h].p[0], heads[h].p[1], heads[h].p[2]);

        // ...and the eye-to-head is not. Whatever the eye offset happens to be,
        // an eye is a few centimetres from the head and nothing else.
        for (int e = 0; e < 2; e++) {
            float px, py, pz, qx, qy, qz, qw, tan[4];
            kl_ovrp_eye_view(e, &px, &py, &pz, &qx, &qy, &qz, &qw, tan);
            XrPosef eye = { {qx, qy, qz, qw}, {px, py, pz} };
            e2h[h][e] = klxr_pose_rel(klxr_space_pose(&view), eye).position;
            float d = sqrtf(e2h[h][e].x * e2h[h][e].x + e2h[h][e].y * e2h[h][e].y +
                            e2h[h][e].z * e2h[h][e].z);
            int near = d < 0.2f;
            fprintf(f, "  %s eye %d to head is %.4f m from it%s\n",
                    near ? "ok  " : "FAIL", e, d,
                    near ? "" : " — the head's own position is in it");
            ok &= near;
        }

        // LOCAL and STAGE are both fixed frames, so they differ by the standing
        // height and by nothing the head does.
        XrPosef local_in_stage = klxr_pose_rel(klxr_space_pose(&stage),
                                               klxr_space_pose(&local));
        ok &= klxr_st_pos(f, "LOCAL in STAGE is the standing height",
                          local_in_stage.position, 0, eh, 0);

        // poseInReferenceSpace, which nothing read at all until SL-16. In VIEW
        // it must be ROTATED by the head, not merely added to it: a metre in
        // front of a head yawed 90 degrees is a metre along -x, not along -z.
        klxr_space ahead = klxr_st_space(KLXR_REF_SPACE_VIEW, 0, 0, -1.0f);
        XrPosef a = klxr_pose_rel(klxr_space_pose(&stage), klxr_space_pose(&ahead));
        float fx = heads[h].p[0] - (h ? 1.0f : 0.0f);
        float fz = heads[h].p[2] - (h ? 0.0f : 1.0f);
        ok &= klxr_st_pos(f, "a space 1 m in front of the head", a.position,
                          fx, heads[h].p[1], fz);
    }

    // The regression itself, stated as one comparison: moving and turning the
    // head must not change where the eyes are relative to it.
    for (int e = 0; e < 2; e++) {
        int same = fabsf(e2h[0][e].x - e2h[1][e].x) <= 1e-4f &&
                   fabsf(e2h[0][e].y - e2h[1][e].y) <= 1e-4f &&
                   fabsf(e2h[0][e].z - e2h[1][e].z) <= 1e-4f;
        fprintf(f, "  %s eye %d to head is unchanged by moving the head\n",
                same ? "ok  " : "FAIL", e);
        ok &= same;
    }
    return ok;
}
