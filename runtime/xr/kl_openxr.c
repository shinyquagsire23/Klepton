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
//   swapchains                           the eye images — the compositor seam
//   the frame loop                       wait/begin/end, locate views and spaces
//
// Two things about the ABI, since neither is obvious from the spec text:
//
//   - Every xr* function returns XrResult, an int32 where 0 is XR_SUCCESS and
//     negatives are failures. Answering a plain "1 for true" here produces a
//     FAILURE code the engine may ignore and managed code trips over three
//     layers away.
//   - Structures are versioned by a `type` enum in their first field and
//     chained through `next`. We never invent a layout: anything we fill in is
//     transcribed from the specification, and anything we have not transcribed
//     is a refusal rather than a partly-populated struct.
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "kl_openxr.h"
#include "kl_jni.h"
#include "kl_ovrp.h"
#include "kl_egl.h"
#include "kl_glfb.h"
#include "kl_env.h"
// XR_KHR_vulkan_enable. Only kl_vulkan.h — never a Vulkan header: every Vulkan
// handle in this file is a void *, which is what keeps it compiling on a
// checkout with no MoltenVK vendored (the stub half of kl_vulkan.c answers
// "not supported" and the extension is then never advertised).
#include "kl_vulkan.h"
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
//     named refusal rather than a wild read.
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
    XR_TYPE_ACTION_STATE_VECTOR2F       = 25,
    XR_TYPE_ACTION_STATE_POSE           = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO      = 28,
    XR_TYPE_ACTION_CREATE_INFO          = 29,
    XR_TYPE_ACTION_SPACE_CREATE_INFO    = 38,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_INTERACTION_PROFILE_STATE   = 53,
    XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO = 54,
    XR_TYPE_ACTION_STATE_GET_INFO       = 58,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO           = 61,
    XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO = 62,
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
    // XR_KHR_vulkan_enable is extension 25, so its three structures sit in the
    // block above GLES's. Open Brush is the guest that needs them.
    XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR      = 1000025000,
    XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR       = 1000025001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR = 1000025002,
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

// XR_KHR_vulkan_enable's binding and requirements. Every Vulkan handle here is
// a `void *` on purpose: a dispatchable Vulkan handle is a pointer on LP64, so
// this file needs no Vulkan header and stays linkable in a build with no
// MoltenVK — kl_vulkan.c owns everything that has to know what they point at.
//
// The two uint32s are NOT padding to be skipped: queueFamilyIndex and
// queueIndex say which queue the app will submit its rendering on, and a
// runtime that ignores them cannot order anything against the app's work.
typedef struct { int32_t type; const void *next;
                 void *instance, *physicalDevice, *device;
                 uint32_t queueFamilyIndex, queueIndex;
               } XrGraphicsBindingVulkanKHR;

typedef struct { int32_t type; void *next;
                 XrVersion minApiVersionSupported,
                           maxApiVersionSupported; } XrGraphicsRequirementsVulkanKHR;

typedef struct { int32_t type; const void *next;
                 int32_t primaryViewConfigurationType; } XrSessionBeginInfo;

typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { float x, y; } XrVector2f;
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
typedef struct { int32_t type; void *next;
                 XrVector2f currentState; XrBool32 changedSinceLastSync;
                 int64_t lastChangeTime; XrBool32 isActive; } XrActionStateVector2f;

// Which physical paths an action is bound to — the resize/no-resize two-call
// shape the enumerators use everywhere else, and answered only for the paths
// that are currently ACTIVE, which is what the spec means by "bound".
typedef struct { int32_t type; const void *next;
                 void *action; } XrBoundSourcesForActionEnumerateInfo;

// A display name for a source path — "Oculus Quest2 Left Controller" and the
// like. The whichComponents bits are the spec's (USER_PATH / PROFILE /
// COMPONENT / UNIQUE_ID); we never claim to know a unique id, so the two bits
// that matter are the first and third.
typedef struct { int32_t type; const void *next;
                 XrPath sourcePath; uint32_t whichComponents;
               } XrInputSourceLocalizedNameGetInfo;
#define XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT            0x00000001
#define XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT 0x00000002
#define XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT           0x00000004
#define XR_INPUT_SOURCE_LOCALIZED_NAME_UNIQUE_ID_BIT           0x00000008

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

// ...and for Vulkan, one VkImage per image. 64 bits wide rather than 32: a
// VkImage is a NON-dispatchable handle, which is a full pointer on LP64 and
// only a uint64 on 32-bit — so the GLES struct's uint32 is a texture NAME and
// this one is an address, and writing the latter into the former's width would
// truncate every handle above 4 GiB into a plausible-looking small one.
typedef struct { int32_t type; void *next; uint64_t image;
               } XrSwapchainImageVulkanKHR;

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

// A flat panel in space: one swapchain image, one pose, one size in metres.
// `eyeVisibility` is BOTH(0) / LEFT(1) / RIGHT(2) — a guest showing different
// pixels to each eye submits two of these rather than one with two views, which
// is why there is no viewCount here.
typedef struct { int32_t type; const void *next;
                 uint64_t layerFlags; void *space;
                 int32_t eyeVisibility;
                 XrSwapchainSubImage subImage;
                 XrPosef pose;
                 struct { float width, height; } size;
               } XrCompositionLayerQuad;

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
    X(xrGetActionStateVector2f)                                                \
    X(xrGetActionStatePose)                                                    \
    X(xrEnumerateBoundSourcesForAction) X(xrGetInputSourceLocalizedName)       \
    /* haptics */                                                              \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback)                           \
    /* extensions — NOT in any import list, reachable only through             \
       xrGetInstanceProcAddr, which is why the "not served" line in that       \
       function exists: it is the only way one of these ever gets named. Each  \
       one below was added because that line named it and the guest then       \
       called the null pointer it got back. */                                 \
    X(xrInitializeLoaderKHR)                                                   \
    X(xrGetOpenGLESGraphicsRequirementsKHR)                                    \
    X(xrGetVulkanGraphicsRequirementsKHR)                                      \
    X(xrGetVulkanInstanceExtensionsKHR) X(xrGetVulkanDeviceExtensionsKHR)      \
    X(xrGetVulkanGraphicsDeviceKHR)                                            \
    X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB)           \
    X(xrRequestDisplayRefreshRateFB)                                           \
    X(xrConvertTimespecTimeToTimeKHR) X(xrConvertTimeToTimespecTimeKHR)         \
    X(xrPerfSettingsSetPerformanceLevelEXT)                                    \
    X(xrSetAndroidApplicationThreadKHR)                                        \
    X(xrEnumerateColorSpacesFB) X(xrSetColorSpaceFB)

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

static void klxr_report_frames(FILE *f);

void kl_openxr_report(FILE *f) {
    unsigned nres = 0, ncall = 0;
    for (int i = 0; i < KLXR_COUNT; i++) {
        nres  += g_xr[i].resolved != 0;
        ncall += g_xr[i].called   != 0;
    }
    fprintf(f, "\n=== OpenXR surface (libklepton_openxr) ===\n");
    fprintf(f, "  %d entry points served; %u resolved by the guest, "
               "%u refused by name\n", KLXR_COUNT, nres, ncall);
    // The frame accounting, which was kept and never printed. An entry-point
    // census says the guest CAN present; only these say whether it DID — and
    // `layers ignored` is the one number that separates "the compositor showed
    // nothing" from "the guest submitted nothing a projection layer could be
    // read out of", which is otherwise a black eye texture with every counter
    // healthy. Through a helper because the session lives further down this
    // file than the report does.
    klxr_report_frames(f);
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
    // The heading must not claim "resolved but never called": that reading
    // lists xrEndFrame on a run whose own log shows xrEndFrame working, and a
    // report contradicting the trace costs more than no report.
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
// Everything not yet built, named — so the guest states which of the forty-six
// it wants.
//
// This aborts rather than returning an error code. An XrResult failure is a
// value the guest is entitled to handle, and XrAppManager does: it takes an
// error path, lands somewhere plausible, and the log shows a tidy shutdown
// naming nothing that was missing.
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
// xrGetInstanceProcAddr, playing the role the ovrp entry table plays: our
// function handing out our own pointers. It is the one entry
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
    int       ext_vulkan;         // ...or XR_KHR_vulkan_enable? (Open Brush)
    int       vk_requirements_queried;   // its own gate, separate from GLES's:
                                         // the spec's requirements-call check is
                                         // per graphics API, and one flag for
                                         // both would let a GLES query unlock a
                                         // Vulkan session.
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
// Why each built-in may be withheld. A per-entry GATE, never arithmetic on the
// array length: a positional "hiding the last entry is count - 1"
// silently hides the previous conditional entry as soon as a second one exists.
// Vulkan is that second one.
enum { KLXR_GATE_ALWAYS = 0,      // unconditional
       KLXR_GATE_REFRESH = 1,     // ...unless KL_XR_REFRESH_EXT=0
       KLXR_GATE_VULKAN = 2 };    // ...only when MoltenVK is actually reachable

static const struct { const char *name; uint32_t version; int gate; } g_extensions[] = {
    { "XR_KHR_opengl_es_enable",       10, KLXR_GATE_ALWAYS },
    { "XR_KHR_android_create_instance", 3, KLXR_GATE_ALWAYS },
    // XR_KHR_convert_timespec_time is the cheapest honest claim in this table:
    // our XrTime IS CLOCK_MONOTONIC nanoseconds (klxr_now), so the conversion is
    // arithmetic and cannot be wrong. Without it the guest logs "not available
    // on this runtime" on EVERY frame — a quarter of a million lines in a 40 s
    // run, which is not just noise: it is the guest's per-frame path doing
    // formatted I/O, and it buried the six lines of the video path that the run
    // existed to produce.
    { "XR_KHR_convert_timespec_time",   1, KLXR_GATE_ALWAYS },
    // XR_FB_display_refresh_rate is the exception to the paragraph above:
    // absence is NOT handled gracefully here. The guest prints
    // "XR_EXT_display_refresh_rate is not available on this runtime" and
    // carries on to "[SVLClientXR] Supported refresh rates was empty!", then
    // publishes that empty list to the Steam host as
    // VTE_AVAILABLE_FRAMETIMES_US. A host told the client can present at no
    // rate at all never starts sending video: the link comes up, the control
    // channel exchanges updates, and the decoder is configured and never handed
    // a single buffer.
    //
    // Claiming it is truthful because it can be answered truthfully: the
    // display frequency is measured (kl_ovrp_set_display_frequency, pushed by
    // the compositor's primeDisplay) and is the SAME number the OVRP side
    // reports, so the two XR APIs cannot disagree about the panel.
    { "XR_FB_display_refresh_rate",     1, KLXR_GATE_REFRESH },
    // XR_KHR_vulkan_enable — Open Brush, and the first guest to need this half
    // of the runtime joined to the Vulkan half.
    //
    // GATED on MoltenVK actually being reachable, and that is the whole reason
    // gates exist: a runtime that names this extension promises four entry
    // points and a session that binds to a VkDevice, and on a checkout with no
    // `make mvk` it can honour none of them. Withheld, the guest fails its own
    // xrCreateInstance with XR_ERROR_EXTENSION_NOT_PRESENT and says so by name
    // in its startup diagnostic — which is a legible stop. Claimed falsely, the
    // guest creates Vulkan objects, hands them to us, and dies somewhere in
    // MoltenVK with nothing naming the cause.
    //
    // Version 8 is the extension's final revision; nothing here reads it, but
    // understating it invites an app to take a compatibility path it does not
    // need.
    { "XR_KHR_vulkan_enable",           8, KLXR_GATE_VULKAN },
    // XR_EXT_performance_settings — JKXR, and it is the counter-example to the
    // paragraph at the top of this table: this guest lists it as a REQUIRED
    // extension in xrCreateInstance rather than probing for it, so withholding
    // it is XR_ERROR_EXTENSION_NOT_PRESENT and the engine exits before it has
    // drawn anything.
    //
    // Claiming it is honest because the whole extension is a HINT. Its one
    // entry point asks the runtime to bias CPU or GPU clocks; there is no such
    // control here — visionOS manages the clocks and does not expose them — so
    // the truthful implementation is to accept the hint, record it and change
    // nothing. That is the same answer a real runtime is permitted to give: the
    // spec makes the level advisory and the notification event optional, so an
    // app cannot distinguish "noted and ignored" from "noted and unhelpful".
    { "XR_EXT_performance_settings",    4, KLXR_GATE_ALWAYS },
    // XR_KHR_android_thread_settings — JKXR again, and required by it for the
    // same reason and with the same shape: one entry point, carrying a hint.
    // The app names a thread by tid and says what it is for (its main thread,
    // its renderer, a worker) so that Android can pin it to a big core and
    // raise its priority.
    //
    // Nothing here can act on it, and that is already this project's recorded
    // position rather than a new one: Process.setThreadPriority is recorded and
    // not applied, because Darwin sets scheduling through pthread QoS on the
    // thread ITSELF and a tid is not a handle we can act through. So this is
    // recorded and named, like the perf hint above it.
    { "XR_KHR_android_thread_settings", 5, KLXR_GATE_ALWAYS },
    // XR_FB_color_space — JKXR, required. See the implementation for why the
    // enumerated list is two entries rather than the extension's eight.
    { "XR_FB_color_space",              3, KLXR_GATE_ALWAYS },
};
#define KLXR_EXT_ALL ((uint32_t)(sizeof g_extensions / sizeof g_extensions[0]))

// KL_XR_REFRESH_EXT=0 hides XR_FB_display_refresh_rate again — the A/B for the
// finding above, and it works by count because that extension is deliberately
// the last entry. Advertising it changes what the client publishes to the
// Steam host, so this is the knob that says whether a change in host behaviour
// came from here.
// KL_XR_EXTRA_EXTENSIONS="<name> <name> ..." appends names to the advertised
// list. It is SCOUTING ONLY and a lie by construction — an extension we name
// here is one we have no entry points for, so a guest that takes us up on it
// resolves NULL or aborts by name. It exists because "what does this guest do
// differently when the runtime claims X?" is otherwise a rebuild per question,
// and for this corpus the answer is usually "nothing", which is worth knowing
// cheaply: VRChat requests exactly one feature extension
// (XR_VALVE_frame_controller_interaction, from Valve's Steam Frame OpenXR
// package) and Unity reports it as unsupported in the startup diagnostic, so
// it is the only thing the guest asks for and does not get. Claiming it is how
// you find out whether anything downstream was keyed on it.
#define KLXR_EXT_EXTRA_MAX 8
static struct { const char *name; uint32_t version; } g_ext_extra[KLXR_EXT_EXTRA_MAX];
static uint32_t g_ext_nextra;
static void klxr_ext_extra_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("KL_XR_EXTRA_EXTENSIONS");
    if (!e || !*e) return;
    char *dup = strdup(e);                  // kept: the names are handed out
    if (!dup) return;
    for (char *t = strtok(dup, " ,\t"); t && g_ext_nextra < KLXR_EXT_EXTRA_MAX;
         t = strtok(NULL, " ,\t")) {
        g_ext_extra[g_ext_nextra].name    = t;
        g_ext_extra[g_ext_nextra].version = 1;
        g_ext_nextra++;
        fprintf(stderr, "  [xr] KL_XR_EXTRA_EXTENSIONS: advertising %s — SCOUTING "
                        "ONLY, nothing implements it\n", t);
    }
}
// The advertised list, RESOLVED once: every built-in whose gate passes, then
// the KL_XR_EXTRA_EXTENSIONS tail. Building it beats computing count and index
// separately — those were two expressions that had to agree about which entries
// were hidden, and the agreement was positional.
static struct { const char *name; uint32_t version; }
       g_ext_live[KLXR_EXT_ALL + KLXR_EXT_EXTRA_MAX];
static uint32_t g_ext_nlive;

static int klxr_ext_gate_open(int gate) {
    switch (gate) {
    case KLXR_GATE_REFRESH: {
        const char *e = getenv("KL_XR_REFRESH_EXT");
        return !(e && !strcmp(e, "0"));
    }
    case KLXR_GATE_VULKAN:
        // KL_XR_VULKAN=0 withholds it even where it works, which restores the
        // pre-bridge configuration EXACTLY: Open Brush's own xrCreateInstance
        // fails with XR_ERROR_EXTENSION_NOT_PRESENT and its startup diagnostic
        // prints `XR_KHR_vulkan_enable (MISSING)`. That is the A/B for "did this
        // come from the bridge?", and it is also the only way to see the old
        // failure again once the bridge works.
        if (!kl_env_on("KL_XR_VULKAN", 1)) return 0;
        // Asked once, here, so the answer cannot change between the
        // enumeration, xrCreateInstance's check and xrGetInstanceProcAddr.
        return kl_vulkan_xr_supported();
    default:
        return 1;
    }
}

static void klxr_ext_resolve(void) {
    static int done;
    if (done) return;
    done = 1;
    klxr_ext_extra_init();
    for (uint32_t i = 0; i < KLXR_EXT_ALL; i++) {
        if (!klxr_ext_gate_open(g_extensions[i].gate)) {
            fprintf(stderr, "  [xr] extension withheld: %s\n", g_extensions[i].name);
            continue;
        }
        g_ext_live[g_ext_nlive].name    = g_extensions[i].name;
        g_ext_live[g_ext_nlive].version = g_extensions[i].version;
        g_ext_nlive++;
    }
    for (uint32_t i = 0; i < g_ext_nextra; i++) {
        g_ext_live[g_ext_nlive].name    = g_ext_extra[i].name;
        g_ext_live[g_ext_nlive].version = g_ext_extra[i].version;
        g_ext_nlive++;
    }
}

static uint32_t klxr_ext_count(void) {
    klxr_ext_resolve();
    return g_ext_nlive;
}
#define KLXR_EXT_COUNT (klxr_ext_count())

// One accessor so the three consumers cannot disagree about what is advertised.
static const char *klxr_ext_at(uint32_t i, uint32_t *version_out) {
    klxr_ext_resolve();
    if (i >= g_ext_nlive) return NULL;
    if (version_out) *version_out = g_ext_live[i].version;
    return g_ext_live[i].name;
}

// Is a name in the advertised list? xrCreateInstance validates the app's
// requested extensions against exactly this, so asking one function keeps the
// answer identical to what was enumerated.
static int klxr_ext_advertised(const char *name) {
    klxr_ext_resolve();
    for (uint32_t i = 0; i < g_ext_nlive; i++)
        if (!strcmp(g_ext_live[i].name, name)) return 1;
    return 0;
}

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
        uint32_t ver = 1;
        const char *nm = klxr_ext_at(i, &ver);
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        snprintf(props[i].extensionName, sizeof props[i].extensionName, "%s", nm);
        props[i].extensionVersion = ver;
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
// ALPHA_BLEND are the passthrough modes, and offering one has the guest leave
// its background transparent for a blend that never happens — a correct RGB
// frame that composites as fully transparent, with no error surface.
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
        int known = klxr_ext_advertised(name);
        fprintf(stderr, "  [xr]   extension: %-40s %s\n", name,
                known ? "enabled" : "NOT PRESENT");
        if (!known) { g_instance.magic = 0; return KLXR_ERROR_EXTENSION_NOT_PRESENT; }
        if (strcmp(name, "XR_KHR_opengl_es_enable") == 0) g_instance.ext_opengl_es = 1;
        if (strcmp(name, "XR_KHR_vulkan_enable") == 0)    g_instance.ext_vulkan = 1;
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
        // itself, and a foveated pass's resolve must stay
        // physical-to-physical.
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
// the floor (the context kl_egl actually creates — a capability set describing
// 3.2 does not make the context 3.2) and 3.2 as the ceiling we answer queries
// for. Overstating the floor
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

// ---------------------------------------------------------- XR_KHR_vulkan_enable
//
// The four entry points an app calls BEFORE it has a session, in this order:
//
//   xrGetVulkanGraphicsRequirementsKHR   which Vulkan versions may be used
//   xrGetVulkanInstanceExtensionsKHR     what to enable on its VkInstance
//   (the app creates its VkInstance)
//   xrGetVulkanGraphicsDeviceKHR         which VkPhysicalDevice to use
//   xrGetVulkanDeviceExtensionsKHR       what to enable on its VkDevice
//   (the app creates its VkDevice, and only then calls xrCreateSession)
//
// All four are answered by kl_vulkan.c, which is the half that knows what a
// VkPhysicalDevice is; this file only translates between OpenXR's spelling and
// that. See kl_vulkan.h for why the two extension lists are legitimately empty.

// The two extension queries share everything but which list they read, so they
// share a body — the spec's buffer contract is fiddly enough (a
// SPACE-DELIMITED single string, NUL-terminated, counted INCLUDING the
// terminator) that having it written twice is how the two would come to differ.
static XrResult klxr_vulkan_ext_string(void *instance, XrSystemId system_id,
                                       const char *list,
                                       uint32_t capacity, uint32_t *count_out,
                                       char *buffer) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (!count_out) return KLXR_ERROR_VALIDATION_FAILURE;
    uint32_t need = (uint32_t)strlen(list) + 1;      // the NUL is counted
    *count_out = need;
    if (capacity == 0) return KLXR_SUCCESS;
    if (capacity < need) return KLXR_ERROR_SIZE_INSUFFICIENT;
    if (!buffer) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(buffer, list, need);
    return KLXR_SUCCESS;
}

static XrResult klxr_GetVulkanInstanceExtensionsKHR(
        void *instance, XrSystemId system_id, uint32_t capacity,
        uint32_t *count_out, char *buffer) {
    return klxr_vulkan_ext_string(instance, system_id,
                                  kl_vulkan_xr_instance_extensions(),
                                  capacity, count_out, buffer);
}

static XrResult klxr_GetVulkanDeviceExtensionsKHR(
        void *instance, XrSystemId system_id, uint32_t capacity,
        uint32_t *count_out, char *buffer) {
    return klxr_vulkan_ext_string(instance, system_id,
                                  kl_vulkan_xr_device_extensions(),
                                  capacity, count_out, buffer);
}

// Which VkPhysicalDevice the app must render with.
//
// The VkInstance is the APP's, passed in — this runtime does not get to assume
// the app's instance is one it has seen, even though here it always is.
static XrResult klxr_GetVulkanGraphicsDeviceKHR(
        void *instance, XrSystemId system_id, void *vk_instance,
        void **vk_physical_device) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (!vk_physical_device) return KLXR_ERROR_VALIDATION_FAILURE;
    void *pd = kl_vulkan_xr_physical_device(vk_instance);
    if (!pd) {
        fprintf(stderr, "  [xr] xrGetVulkanGraphicsDeviceKHR: no physical device "
                        "from the app's VkInstance %p\n", vk_instance);
        return KLXR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    fprintf(stderr, "  [xr] xrGetVulkanGraphicsDeviceKHR -> VkPhysicalDevice %p\n", pd);
    *vk_physical_device = pd;
    return KLXR_SUCCESS;
}

// The version gate, and the Vulkan twin of the GLES one above: without this call
// xrCreateSession must fail with XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING.
// Its own flag, not the GLES one — see klxr_instance.
static XrResult klxr_GetVulkanGraphicsRequirementsKHR(
        void *instance, XrSystemId system_id,
        XrGraphicsRequirementsVulkanKHR *reqs) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (system_id != KLXR_SYSTEM_ID) return KLXR_ERROR_SYSTEM_INVALID;
    if (!reqs) return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrGetVulkanGraphicsRequirementsKHR", reqs->next);
    unsigned lo_maj, lo_min, hi_maj, hi_min;
    kl_vulkan_xr_api_range(&lo_maj, &lo_min, &hi_maj, &hi_min);
    reqs->type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    reqs->minApiVersionSupported = XR_MAKE_VERSION(lo_maj, lo_min, 0);
    reqs->maxApiVersionSupported = XR_MAKE_VERSION(hi_maj, hi_min, 0);
    g_instance.vk_requirements_queried = 1;
    fprintf(stderr, "  [xr] xrGetVulkanGraphicsRequirementsKHR: Vulkan %u.%u .. %u.%u\n",
            lo_maj, lo_min, hi_maj, hi_min);
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
// Same shape as the Choreographer and the looper: on Android something else
// drives the pump, and here the runtime IS the something else. The transitions
// are queued eagerly rather than waiting for a frontend to say the headset is
// worn — there is no such signal on the host, and an idle guest looks exactly
// like a hung one.
enum { KLXR_MAGIC_SESSION = 0x584b4c53 /* 'XKLS' */ };
enum { KLXR_EVENT_QUEUE = 16 };

// Which graphics API a session is bound with — see `gfx` below.
enum { KLXR_GFX_NONE = 0, KLXR_GFX_GLES = 1, KLXR_GFX_VULKAN = 2 };

typedef struct {
    uint32_t magic;
    klxr_instance *instance;
    XrSystemId systemId;
    // Which graphics API this session was bound with. Not derivable from the
    // instance's enabled extensions: an app may enable both and bind one, and
    // every place downstream that hands an image back has to know which
    // spelling to use.
    int   gfx;
    void *egl_display, *egl_config, *egl_context;   // the guest's own GL binding
    // ...and the guest's own Vulkan binding. All four handles are the APP's —
    // it created them, through kl_vulkan's synthetic loader.
    void *vk_instance, *vk_phys, *vk_device;
    uint32_t vk_queue_family, vk_queue_index;
    int   state;                  // the last state we POSTED, not the next one
    int   running;                // between xrBeginSession and xrEndSession
    int   exit_requested;
    int   action_sets_attached;   // xrAttachSessionActionSets is once, and final
    int      frame_begun;         // between xrBeginFrame and xrEndFrame
    int64_t  frame_predicted_time;// what the last xrWaitFrame promised
    uint64_t frames_waited, frames_ended, layers_ignored, layers_quad;
    // The per-layer composite's own census: how many frames carried N
    // projection layers, and per layer index how many carried it at all against
    // how many got it as far as a compositor slot. This is the one measurement
    // that separates "the guest changes its layer set while it runs" from "we
    // are dropping layers" — two causes of one symptom (a picture alternating
    // between two states) that no other line here tells apart.
    uint64_t proj_hist[9];
    uint64_t proj_present[8], proj_placed[8];
    // Pending events, in order. Each carries a KIND as well as a payload (a
    // state, for a session-state change; unused otherwise): a queue of bare
    // states cannot deliver the second event type — the interaction profile
    // changing, which is how an app learns a controller appeared — and does not
    // say so.
    struct { int kind, state; } queue[KLXR_EVENT_QUEUE];
    int   qhead, qcount;
} klxr_session;

enum { KLXR_EV_SESSION_STATE = 0, KLXR_EV_INTERACTION_PROFILE = 1 };

static klxr_session g_session;

static void klxr_report_frames(FILE *f) {
    if (!g_session.frames_waited && !g_session.frames_ended &&
        !g_session.layers_ignored && !g_session.layers_quad) return;
    // The two counts are the whole of what a frame's layer list came to: a quad
    // reaches the compositor's overlay list, anything else reaches nothing. They
    // are separate because "the picture is a panel we placed" and "the picture
    // was dropped" look identical from every other line here.
    fprintf(f, "  frames: %llu waited, %llu ended; %llu quad layer(s) composited, "
               "%llu other non-projection layer(s) ignored\n",
            (unsigned long long)g_session.frames_waited,
            (unsigned long long)g_session.frames_ended,
            (unsigned long long)g_session.layers_quad,
            (unsigned long long)g_session.layers_ignored);
    uint64_t any = 0;
    for (int i = 0; i < 9; i++) any += g_session.proj_hist[i];
    if (!any) return;
    // Frames by layer count. More than one non-zero bucket IS the guest
    // changing its layer set from frame to frame, and every layer it stops
    // submitting is a region of the picture that stops being drawn — which is
    // seen as flicker and is a faithful composite of what arrived.
    fprintf(f, "  projection layers per frame:");
    for (int i = 0; i < 9; i++)
        if (g_session.proj_hist[i])
            fprintf(f, " %d->%llu", i, (unsigned long long)g_session.proj_hist[i]);
    fprintf(f, "\n");
    // ...and per layer, submitted against reached-a-slot. A gap between the two
    // is ours; equality with a moving count is the guest's.
    for (int i = 0; i < 8; i++) {
        if (!g_session.proj_present[i]) continue;
        fprintf(f, "    layer %d: %llu frame(s) submitted, %llu placed%s\n", i,
                (unsigned long long)g_session.proj_present[i],
                (unsigned long long)g_session.proj_placed[i],
                g_session.proj_placed[i] < g_session.proj_present[i]
                    ? "  <- some frames had no compositor slot for this layer" : "");
    }
}

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
// it against this list, picks what it can do, and tells the host. An empty list
// is not a neutral answer to that negotiation — see the gate above.
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


// ---------------------------------------------------------- XR_FB_color_space
//
// The app declares which colour space its SUBMITTED CONTENT is in, so the
// compositor can convert. Required by JKXR at instance creation, like the two
// below it.
//
// We enumerate exactly what this compositor genuinely does: UNMANAGED (pass the
// texture through) and REC709, which is sRGB primaries and transfer — the
// assumption the composite pass already encodes (it does the sRGB decode
// itself). Claiming Rift, Quest or P3 would promise a gamut conversion no pass
// in this project performs.
//
// A guest that sets one of the other spaces is ACCEPTED and told so by name,
// rather than refused. Refusing is the spec-correct answer for a space we do
// not support, and it is the wrong one here: an app that treats the failure as
// fatal loses the whole run over a gamut approximation, and the visible cost of
// treating Quest's space as sRGB is slightly off saturation, not a broken
// frame. Naming it means the day someone asks why the colours are a little
// flat, the answer is in the log rather than in this comment.
enum { KLXR_COLOR_SPACE_UNMANAGED = 0, KLXR_COLOR_SPACE_REC709 = 2 };

static XrResult klxr_EnumerateColorSpacesFB(void *session, uint32_t capacity,
                                            uint32_t *count_out, int *spaces) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    XrResult r = klxr_two_call(capacity, count_out, 2);
    if (r == KLXR_SUCCESS && capacity >= 2 && spaces) {
        spaces[0] = KLXR_COLOR_SPACE_UNMANAGED;
        spaces[1] = KLXR_COLOR_SPACE_REC709;
    }
    return r;
}

static XrResult klxr_SetColorSpaceFB(void *session, int space) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    static const char *const NAMES[] = { "unmanaged", "rec2020", "rec709",
                                         "rift-cv1", "rift-s", "quest", "p3",
                                         "adobe-rgb" };
    const char *nm = (space >= 0 && space <= 7) ? NAMES[space] : "?";
    if (space == KLXR_COLOR_SPACE_UNMANAGED || space == KLXR_COLOR_SPACE_REC709) {
        fprintf(stderr, "  [xr] colour space: %s\n", nm);
        return KLXR_SUCCESS;
    }
    fprintf(stderr, "  [xr] colour space: %s — not one this compositor converts "
                    "from; treated as sRGB (accepted, so a gamut approximation "
                    "does not end the run)\n", nm);
    return KLXR_SUCCESS;
}


// ---------------------------------------------------------- XR_EXT_performance_settings
//
// The extension's whole surface: one call, asking the runtime to bias a domain
// (CPU or GPU) towards a level (power-savings, sustained-low, sustained-high,
// boost). It is ADVISORY in the spec, and there is nothing behind it here —
// visionOS owns the clocks and exposes no equivalent — so this records the hint
// and returns success.
//
// Named on every change rather than counted silently, because the levels a
// guest asks for are a readable description of what it thinks it is doing: JKXR
// raises the GPU domain when it enters a level and drops it in menus, so this
// line is a free marker for where the engine believes it is.
enum { KLXR_PERF_DOMAIN_CPU = 1, KLXR_PERF_DOMAIN_GPU = 2 };

static XrResult klxr_PerfSettingsSetPerformanceLevelEXT(void *session,
                                                        int domain, int level) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    static const char *const LEVELS[] = { "?", "power-savings", "sustained-low",
                                          "sustained-high", "boost" };
    static int last_cpu = -1, last_gpu = -1;
    int *last = domain == KLXR_PERF_DOMAIN_CPU ? &last_cpu
              : domain == KLXR_PERF_DOMAIN_GPU ? &last_gpu : NULL;
    if (!last) return KLXR_ERROR_VALIDATION_FAILURE;
    if (*last == level) return KLXR_SUCCESS;
    *last = level;
    fprintf(stderr, "  [xr] perf hint: %s -> %s (advisory; visionOS owns the "
                    "clocks, nothing is changed)\n",
            domain == KLXR_PERF_DOMAIN_CPU ? "CPU" : "GPU",
            (level >= 1 && level <= 4) ? LEVELS[level] : "?");
    return KLXR_SUCCESS;
}


// ---------------------------------------------------------- XR_KHR_android_thread_settings
//
// "This tid is my renderer thread, treat it accordingly." Recorded and not
// acted on, for the reason in the extension table: Darwin has no door that
// changes another thread's scheduling by tid — QoS is set by the thread on
// itself — so the only implementation available would be to lie.
//
// The tid IS worth printing. This guest spawns its render thread inside
// onCreate and nothing else names it, so the line joins a tid in `sample <pid>`
// output to the role the engine believes that thread has.
static XrResult klxr_SetAndroidApplicationThreadKHR(void *session, int type,
                                                    uint32_t tid) {
    if (!klxr_sess(session)) return KLXR_ERROR_HANDLE_INVALID;
    static const char *const KINDS[] = { "?", "app-main", "app-worker",
                                         "renderer-main", "renderer-worker" };
    fprintf(stderr, "  [xr] thread hint: tid %u is the guest's %s "
                    "(recorded; Darwin sets scheduling on the thread itself)\n",
            tid, (type >= 1 && type <= 4) ? KINDS[type] : "?");
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
// Choreographer runs on one: two monotonic clocks differ by an offset, and a
// frame delta computed across the pair is that offset rather than a duration. CLOCK_MONOTONIC is what System.nanoTime() already answers here.
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
    const XrGraphicsBindingVulkanKHR *vk = NULL;
    for (const void *n = info->next; n; ) {
        int32_t type = *(const int32_t *)n;
        if (type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR)
            gl = (const XrGraphicsBindingOpenGLESAndroidKHR *)n;
        else if (type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
            vk = (const XrGraphicsBindingVulkanKHR *)n;
        else
            fprintf(stderr, "  [xr] xrCreateSession: chained struct type %d — ignored\n", type);
        n = *(const void *const *)((const char *)n + 8);
    }
    // Exactly one binding. Both is not a richer request, it is a contradiction
    // about which API the swapchain images must be handed back in, and the
    // spec has no way to express it — so it is refused rather than resolved by
    // precedence, which would silently pick for the guest.
    if (gl && vk) {
        fprintf(stderr, "  [xr] xrCreateSession: BOTH a GLES and a Vulkan binding "
                        "chained — refusing rather than choosing\n");
        return KLXR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (!gl && !vk) {
        fprintf(stderr, "  [xr] xrCreateSession: no graphics binding chained\n");
        return KLXR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    // The requirements gate is per graphics API, and so is the check.
    if (gl && !inst->gl_requirements_queried)
        return KLXR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    if (vk && !inst->vk_requirements_queried)
        return KLXR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;

    memset(&g_session, 0, sizeof g_session);
    g_session.magic = KLXR_MAGIC_SESSION;
    g_session.instance = inst;
    g_session.systemId = info->systemId;
    g_session.state = KLXR_SESSION_STATE_UNKNOWN;

    if (gl) {
        g_session.gfx = KLXR_GFX_GLES;
        g_session.egl_display = gl->display;
        g_session.egl_config  = gl->config;
        g_session.egl_context = gl->context;
        fprintf(stderr, "  [xr] xrCreateSession: EGLDisplay %p config %p context %p\n",
                gl->display, gl->config, gl->context);
    } else {
        g_session.gfx = KLXR_GFX_VULKAN;
        g_session.vk_instance = vk->instance;
        g_session.vk_phys     = vk->physicalDevice;
        g_session.vk_device   = vk->device;
        g_session.vk_queue_family = vk->queueFamilyIndex;
        g_session.vk_queue_index  = vk->queueIndex;
        fprintf(stderr, "  [xr] xrCreateSession: VkInstance %p physical %p device %p "
                        "queue %u.%u\n",
                vk->instance, vk->physicalDevice, vk->device,
                vk->queueFamilyIndex, vk->queueIndex);
    }

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
// queue is empty, which is the normal answer most frames. The return convention
// runs an unusual direction here: a POSITIVE result is still success, so a
// caller testing `result == XR_SUCCESS` rather than `>= 0` reads an empty queue
// as an event. That is the guest's business, but it is why this returns the
// specified code rather than an error.
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
// behind the visionOS temporal doubling. Wiring a second
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
// The list of resolved entry points cannot answer this: a guest resolves plenty
// it never calls, and `xrLocateViews` appearing there says nothing about which
// space it passes.
// The distinction is the whole question. A guest that locates its views in VIEW
// space is asking "where are the eyes relative to the head" — an eye-to-head —
// and one that locates them in LOCAL is asking where they are in the world.
// Those two answers differ by the standing height, and answering the first with
// the second puts that height into the eye-to-head: a lever arm the head then
// swings around instead of turning in place.
//
// Returns 1 the first time a combination is seen, and nothing else: the caller
// prints the line, in ONE fprintf, once it also has the answer it gave. The
// guest logs from several threads, so a line split across the computation is a
// line split in the log.
// ...once per (call, space, base), AND THEN RARELY AGAIN.
//
// Once answers WHICH spaces the guest asks about, but not whether the ANSWERS
// are right: the first call happens before the head has moved, so every position
// in the census is 0 and a leak of the head's own position is indistinguishable
// from no leak.
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
// VIEW is a whole POSE here, not a y displacement. A guest passes VIEW as the
// base to xrLocateViews to ask for its eye-to-head, and a space modelled as a
// scalar answers with the head's own position folded in — the eye then sits
// 1.7 m from the point it is rotated about and the head swings through an arc
// instead of turning in place. The census above settles which spaces a guest
// actually asks about; do not assume the two static ones.
//
// Getting the static two wrong is not subtle in the end result and is very
// subtle here: an app that places its UI in LOCAL and is answered STAGE puts
// every panel on the floor.
// Which hand an ACTION space follows, and whether it is a GRIP or an AIM pose.
// Defined with the actions, below, because it has to look inside one. Returns
// -1 when the space is anchored to nothing we can locate.
static int klxr_action_space_hand(const klxr_space *sp, int *is_aim);

// The aim pose, as a rotation off the grip pose. Measured on hardware, not
// derived here: 37 degrees, close to the angle a Touch controller needs.
//
// OpenXR gives a controller two poses: the GRIP (the hilt — where the hand is)
// and the AIM (the ray — where the user is pointing). Steam Link asks for both
// by name: `pamir-stream-pose` binds grip and `ui_pointer_pose` binds aim, so
// the in-headset UI pointer is the aim one, and with the two collapsed the
// laser leaves the hand at the hilt's angle instead of the pointing angle.
// KleptonControllers' hilt frame does not make the two coincide, close as its
// -Z is to the direction the hilt points.
//
// Sign convention: R_x(θ) takes the forward vector (0,0,-1) to (0, sinθ,
// -cosθ), so positive pitches forward UP and negative pitches it down.
// POSITIVE on the grip, confirmed by eye on a headset streaming from SteamVR.
//
// The guest's own controller_config.json does not predict that sign and looks
// as though it should: its per-profile hilt rotations are all negative about
// the same axis (-20.6 Touch, -10 Pico, -5 Vive). Those are the guest's
// grip-to-DEVICE offsets, applied on its side to a pose it already has; this is
// the correction from the frontend's hilt frame INTO the grip pose the guest
// expects, and the two run opposite ways. Do not re-derive the sign there.
//
// OpenXR path only, so it does not touch Beat Saber, which
// speaks OVRPlugin and never resolves a single xr* entry point (its own
// end-of-run report reads `0 resolved by the guest`). The rotation is applied
// to kl_openxr's local copy of the pose, not written back into kl_ovrp, so the
// two guests cannot be made to disagree by this knob. If this ever needs to
// move into kl_ovrp, it needs a per-guest split at that moment — the shared
// seam is the reason, not the knob.
#define KLXR_GRIP_PITCH_DEFAULT (37.0f)
#define KLXR_AIM_PITCH_DEFAULT  (-37.0f)
static void klxr_pitch_about_x(XrPosef *p, float degrees) {
    if (degrees == 0.0f) return;
    float half = degrees * 0.5f * 3.14159265358979f / 180.0f;
    XrQuaternionf rx = { sinf(half), 0, 0, cosf(half) };
    p->orientation = klxr_qmul(p->orientation, rx);
}

// The two corrections, applied to every action-space pose. They answer
// different questions and stay separate; each applies to its OWN pose, and a
// correction applied to the aim pose alone leaves the controller the user is
// looking at — the grip — untouched, which reads as the knob doing nothing.
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
// Read once and SAY so, which is why this is split out rather than being a lazy
// init inside the corrector: the corrector only runs while a hand is tracked, so
// on a run without a frontend — every host run — it never executes and the log
// never states which pitch is in force. This pair is the one a person A/Bs
// against a picture.
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

// ...and the POSITION half of the same correction, in the grip's own frame.
//
// **A pitch alone rotates where the controller points and not where it
// PIVOTS**, and that is a different error with a different symptom. The frame
// is rotated about the origin the platform gave us, so a point that should have
// stayed put — the centre of the hilt, inside the closed fist — swings on an
// arc of the pitch angle instead. Reported from a headset on JKXR: "rolling my
// wrist has a very large arc, and the saber sits near my knuckles rather than
// grasped correctly", against Beat Saber where "the saber travels directly
// through the curl of my closed fist".
//
// Beat Saber gets both halves: the frontend's Sense correction carries a
// rotation AND a position nudge (KLSenseTune's `pos`, applied in the grip
// frame), and adjusting that position is what fixed the pivot there and on
// BONELAB. This path had only the rotation, so an OpenXR guest received a frame
// turned 37 degrees about an origin nobody moved.
//
// Zero by default, so nothing changes until a device run says what it should
// be — one run per candidate, no rebuild, exactly as KL_SENSE_POS is swept on
// the other path. `_L`/`_R` override per hand, which is what a chiral error
// needs.
static XrVector3f klxr_grip_vec(int hand, const char *base, const char *l,
                                const char *r);

static XrVector3f klxr_grip_pos(int hand) {
    static int init;
    static XrVector3f off[2];
    if (!init) {
        init = 1;
        for (int h = 0; h < 2; h++)
            off[h] = klxr_grip_vec(h, "KL_XR_GRIP_POS", "KL_XR_GRIP_POS_L",
                                   "KL_XR_GRIP_POS_R");
    }
    return off[(unsigned)hand > 1 ? 0 : hand];
}

// WHERE the pitch turns, in the grip's own frame — the point that should not
// move when the correction is applied.
//
// The pitch is needed: with it at zero this guest reads as "a gun grip rather
// than the sword grip the game is expecting", so the angle is right in kind.
// What is wrong is the CENTRE. `klxr_pitch_about_x` turns the orientation and
// leaves the position where the platform put it, so the frame pivots about the
// tracked origin — somewhere back at the wrist — and everything the guest draws
// from that pose swings on an arc of the pitch angle. The hilt ends up out by
// the knuckles instead of through the closed fist.
//
// So state the pivot instead of solving for a translation. This is a POINT you
// can estimate by looking at your hand — "the hilt centre is about six
// centimetres forward and two down from where the controller is tracked" —
// where the equivalent translation is a 3-DOF sweep with no physical meaning
// and a different right answer for every pitch angle. The translation that
// holds C still falls out of it:
//
//     P' = P + Q·C - (Q·R)·C
//
// with Q the orientation before the pitch and R the pitch. Zero is exactly the
// old behaviour, so nothing moves until this is set.
//
// Grip-frame axes, for reading a result: -Z is the pointing direction, +Y up,
// +X to the right — so a hilt centre ahead of the tracked origin has NEGATIVE
// z, and `_L`/`_R` differ only for a chiral error.
static XrVector3f klxr_grip_vec(int hand, const char *base, const char *l,
                                const char *r) {
    float v[3] = { 0, 0, 0 };
    const char *name = hand == 0 ? l : r;
    const char *e = kl_env_str(name, NULL);
    if (!e) { name = base; e = kl_env_str(name, NULL); }
    if (e && sscanf(e, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
        fprintf(stderr, "  [xr] %s: hand %d %.3f %.3f %.3f m in the grip's own "
                        "frame\n", name, hand, (double)v[0], (double)v[1],
                (double)v[2]);
        return (XrVector3f){ v[0], v[1], v[2] };
    }
    return (XrVector3f){ 0, 0, 0 };
}

static XrVector3f klxr_grip_pivot(int hand) {
    static int init;
    static XrVector3f c[2];
    if (!init) {
        init = 1;
        for (int h = 0; h < 2; h++)
            c[h] = klxr_grip_vec(h, "KL_XR_GRIP_PIVOT", "KL_XR_GRIP_PIVOT_L",
                                 "KL_XR_GRIP_PIVOT_R");
    }
    return c[(unsigned)hand > 1 ? 0 : hand];
}

static void klxr_pose_corrections(XrPosef *p, int is_aim, int hand) {
    float grip_pitch, aim_pitch;
    klxr_pitches(&grip_pitch, &aim_pitch);
    XrQuaternionf before = p->orientation;
    klxr_pitch_about_x(p, grip_pitch);
    if (is_aim) klxr_pitch_about_x(p, aim_pitch);
    // Hold the pivot still across whatever the two pitches just did.
    XrVector3f c = klxr_grip_pivot(hand);
    if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f) {
        XrVector3f was = klxr_qrot(before, c);
        XrVector3f now = klxr_qrot(p->orientation, c);
        p->position.x += was.x - now.x;
        p->position.y += was.y - now.y;
        p->position.z += was.z - now.z;
    }
    // AFTER the pitch, and in the rotated frame: the offset says where the hilt
    // centre is relative to the corrected orientation, which is the frame the
    // guest is about to draw a controller in.
    XrVector3f o = klxr_grip_pos(hand);
    if (o.x != 0.0f || o.y != 0.0f || o.z != 0.0f) {
        XrVector3f d = klxr_qrot(p->orientation, o);
        p->position.x += d.x;
        p->position.y += d.y;
        p->position.z += d.z;
    }
}

// ...and what that leaves the guest holding, in the same units and the same
// shape kl_ovrp prints what it PUBLISHED (kl_ovrp_ctrl_trace). The two lines
// side by side are the whole of the controller-alignment question: the
// frontend applies a convention offset when it publishes, this path adds its
// own to a local copy, and until both were printed nothing said what the
// difference between an OVRPlugin guest's controller and an OpenXR guest's
// actually was.
static void klxr_ctrl_trace(const XrPosef *p, int hand, int is_aim) {
    if (hand < 0 || hand > 1) return;
    float x = p->orientation.x, y = p->orientation.y;
    float z = p->orientation.z, w = p->orientation.w;
    const float R = 57.29577951308232f;
    float sx = 2.0f * (w * x + y * z), cx = 1.0f - 2.0f * (x * x + y * y);
    float sy = 2.0f * (w * y - z * x);
    float sz = 2.0f * (w * z + x * y), cz = 1.0f - 2.0f * (y * y + z * z);
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    float e[3] = { atan2f(sx, cx) * R, asinf(sy) * R, atan2f(sz, cz) * R };
    float pos[3] = { p->position.x, p->position.y, p->position.z };
    kl_ovrp_ctrl_trace(is_aim ? "openxr aim" : "openxr grip", hand, e, pos);
}

// `motion_known` reports whether lin/ang are a measurement — a frontend that
// publishes pose only has them derived (kl_ovrp), and the first sample after a
// gap has no basis at all. That case must reach the guest as velocityFlags == 0
// and not as a velocity of zero, which asserts the controller is stationary.
static XrPosef klxr_space_pose_ex(const klxr_space *sp, int *tracked,
                                  float *lin, float *ang, int *motion_known) {
    XrPosef base = { {0, 0, 0, 1}, {0, 0, 0} };   // STAGE, and the tracking space
    if (tracked) *tracked = 1;
    if (motion_known) *motion_known = 0;
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
        klxr_pose_corrections(&base, is_aim, hand);
        klxr_ctrl_trace(&base, hand, is_aim);
        if (lin) memcpy(lin, v, sizeof v);
        if (ang) memcpy(ang, a, sizeof a);
        if (motion_known) *motion_known = kl_ovrp_hand_motion_known(hand);
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
    return klxr_space_pose_ex(sp, NULL, NULL, NULL, NULL);
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
// is what leaves VTE_PROPS_STATIC_L/_R unpublished: a lookup keyed on a device
// identity answered wrong, breaking a feature two subsystems away.
//
// Touch is also the CONSISTENT answer rather than merely a plausible one: this
// shim presents a Quest 2 everywhere else (Build.MODEL, Build.PRODUCT
// "hollywood", ovrp_GetSystemHeadsetType), and the guest's own oculus entry
// reads "Oculus Quest2 (Left Controller)" / "oculus_touch". Answering a
// different profile would be the inconsistent act.
//
// It is not, however, the ONLY profile we can drive, and it cannot be
// hardcoded: VRChat NEVER suggests the Touch profile. A
// Steam Frame build binds exclusively under /interaction_profiles/valve/
// frame_controller_valve (48 bindings), and with Touch hardcoded as active
// every one of them decoded "inactive profile" — so VRChat had controllers and
// nobody was told. The active profile is therefore decided PER RUN: which
// driveable profile the guest actually suggested, preferring Touch (the
// honest answer for Quest-shaped hardware) and falling back to the Valve frame
// (the only profile VRChat knows). klxr_profile_driveable is the whole list of
// profiles whose inputs we can produce; any guest profile outside it stays an
// inactive suggestion, exactly as before.
#define KLXR_ACTIVE_PROFILE "/interaction_profiles/oculus/touch_controller"
#define KLXR_VALVE_PROFILE  "/interaction_profiles/valve/frame_controller_valve"

// The profile the CURRENT run answers as bound, decided from the guest's own
// suggestions (see the note above). Empty until one is suggested; the getter
// falls back to Touch. Never both at once — only one active profile may exist
// per top-level path, and the two are never both suggested by one guest.
static char g_active_profile[96];

static int klxr_profile_driveable(const char *profile) {
    return strcmp(profile, KLXR_ACTIVE_PROFILE) == 0 ||
           strcmp(profile, KLXR_VALVE_PROFILE) == 0;
}

// What one binding path reads, once decoded.
enum {
    KLXR_SRC_NONE = 0,
    KLXR_SRC_BUTTON,          // a bit of the RAW buttons word
    KLXR_SRC_TOUCH,           // ...of the RAW touches word
    KLXR_SRC_INDEX_TRIGGER,
    KLXR_SRC_HAND_TRIGGER,
    KLXR_SRC_STICK_X,
    KLXR_SRC_STICK_Y,
    KLXR_SRC_STICK_VEC,       // the whole stick, read as a Vector2f
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
//   /input/trigger/click    — analog-only trigger, no separate press bit
//   /input/bumper/(click|touch) — the Valve frame's shoulder button; no
//                                shoulder exists on any source here
//
// Two profiles share this table, and the same suffix can mean different hands
// in each (the Valve frame controller is NOT a Quest controller):
//   oculus/touch_controller — A/B are the right hand, X/Y the left, x/y on the
//        right (and a/b on the left) are absent and stay unbound.
//   valve/frame_controller_valve — VRChat binds the RIGHT face buttons as
//        y(top)/b(outside)/a(bottom)/x(inside) and the LEFT as a dpad. A
//        Quest-shaped hand has two face buttons, not four, so the mapping is
//        deliberately lossy (decided with the user, 2026-08-14): the two
//        RIGHT buttons collapse onto A/B and the dpad directions onto X/Y,
//        because the face buttons are mostly menu garnish and the sticks,
//        triggers and grips are what VRChat actually uses.
static const struct { const char *suffix; int kind; uint32_t bit[2]; }
g_xr_sources[] = {
    // oculus: right hand. valve: right hand again (a = bottom, b = outside) —
    // the two profiles AGREE here, which is why these rows never changed.
    { "/input/a/click",          KLXR_SRC_BUTTON, { 0, KL_OVRP_RAW_A } },
    { "/input/b/click",          KLXR_SRC_BUTTON, { 0, KL_OVRP_RAW_B } },
    // oculus: x/y are the LEFT hand and the right stays unbound. valve: y and
    // x are the RIGHT hand's top/inside buttons, aliased onto the right
    // controller's B and A. Both readings are served by one row: the left bit
    // is the oculus one and the right bit is the valve one.
    { "/input/x/click",          KLXR_SRC_BUTTON, { KL_OVRP_RAW_X, KL_OVRP_RAW_A } },
    { "/input/y/click",          KLXR_SRC_BUTTON, { KL_OVRP_RAW_Y, KL_OVRP_RAW_B } },
    { "/input/menu/click",       KLXR_SRC_BUTTON, { KL_OVRP_RAW_START,
                                                    KL_OVRP_RAW_START } },
    { "/input/system/click",     KLXR_SRC_BUTTON, { KL_OVRP_RAW_BACK,
                                                    KL_OVRP_RAW_BACK } },
    { "/input/thumbstick/click", KLXR_SRC_BUTTON, { KL_OVRP_RAW_LTHUMBSTICK,
                                                    KL_OVRP_RAW_RTHUMBSTICK } },
    // valve only: the left hand's dpad. The two dpad directions collapse onto
    // the two left face buttons (Y is the top button on the left, X the lower)
    // — see the mapping note above.
    { "/input/dpad_up/click",    KLXR_SRC_BUTTON, { KL_OVRP_RAW_Y, 0 } },
    { "/input/dpad_down/click",  KLXR_SRC_BUTTON, { KL_OVRP_RAW_X, 0 } },
    { "/input/dpad_left/click",  KLXR_SRC_BUTTON, { KL_OVRP_RAW_X, 0 } },
    { "/input/dpad_right/click", KLXR_SRC_BUTTON, { KL_OVRP_RAW_Y, 0 } },
    { "/input/view/click",       KLXR_SRC_BUTTON, { KL_OVRP_RAW_BACK,
                                                    KL_OVRP_RAW_BACK } },
    // valve only: squeeze/click. Deliberate for vive (never in the active
    // profile), served now because valve is driveable and VRChat's grip press
    // is a real action. The only grip signal we have is the hand trigger's
    // own threshold bit, which is what "fully squeezed" means on this side.
    { "/input/squeeze/click",    KLXR_SRC_BUTTON, { KL_OVRP_RAW_LHAND_TRIGGER,
                                                    KL_OVRP_RAW_RHAND_TRIGGER } },
    { "/input/a/touch",          KLXR_SRC_TOUCH,  { 0, KL_OVRP_RAW_A } },
    { "/input/b/touch",          KLXR_SRC_TOUCH,  { 0, KL_OVRP_RAW_B } },
    { "/input/x/touch",          KLXR_SRC_TOUCH,  { KL_OVRP_RAW_X, KL_OVRP_RAW_A } },
    { "/input/y/touch",          KLXR_SRC_TOUCH,  { KL_OVRP_RAW_Y, KL_OVRP_RAW_B } },
    { "/input/dpad_up/touch",    KLXR_SRC_TOUCH,  { KL_OVRP_RAW_Y, 0 } },
    { "/input/dpad_down/touch",  KLXR_SRC_TOUCH,  { KL_OVRP_RAW_X, 0 } },
    { "/input/dpad_left/touch",  KLXR_SRC_TOUCH,  { KL_OVRP_RAW_X, 0 } },
    { "/input/dpad_right/touch", KLXR_SRC_TOUCH,  { KL_OVRP_RAW_Y, 0 } },
    { "/input/view/touch",       KLXR_SRC_TOUCH,  { KL_OVRP_RAW_BACK,
                                                    KL_OVRP_RAW_BACK } },
    { "/input/menu/touch",       KLXR_SRC_TOUCH,  { KL_OVRP_RAW_START,
                                                    KL_OVRP_RAW_START } },
    { "/input/squeeze/touch",    KLXR_SRC_TOUCH,  { KL_OVRP_RAW_LHAND_TRIGGER,
                                                    KL_OVRP_RAW_RHAND_TRIGGER } },
    { "/input/trigger/touch",    KLXR_SRC_TOUCH,  { KL_OVRP_RAW_LINDEX_TRIGGER,
                                                    KL_OVRP_RAW_RINDEX_TRIGGER } },
    { "/input/thumbstick/touch", KLXR_SRC_TOUCH,  { KL_OVRP_RAW_LTHUMBSTICK,
                                                    KL_OVRP_RAW_RTHUMBSTICK } },
    { "/input/trigger/value",    KLXR_SRC_INDEX_TRIGGER, { 0, 0 } },
    { "/input/squeeze/value",    KLXR_SRC_HAND_TRIGGER,  { 0, 0 } },
    { "/input/thumbstick/x",     KLXR_SRC_STICK_X, { 0, 0 } },
    { "/input/thumbstick/y",     KLXR_SRC_STICK_Y, { 0, 0 } },
    // valve only: the whole-stick path, which is what a Vector2f action binds
    // to. Valve does not split the stick into x/y paths; xrGetActionStateVector2f
    // reads this kind, and it is why that entry point had to exist.
    { "/input/thumbstick",       KLXR_SRC_STICK_VEC, { 0, 0 } },
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
        case KLXR_SRC_STICK_VEC:     return "thumbstick";
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
    float    value2[2];    // the Y component of a KLXR_SRC_STICK_VEC action
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
// A guest suggests bindings for every controller it knows (Steam Link does
// six, VRChat exactly one), and only the active profile's may be honoured:
// binding an action to the vive `squeeze/click` it also offers would have a
// Touch controller reporting a control it does not have. So the profile test is
// a correctness rule, not a filter for tidiness — and it is why an action
// bound ONLY under another profile correctly ends up inactive. Which profile
// is active is decided HERE, from what the guest actually suggested (see the
// note above g_active_profile), not from a constant.
static XrResult klxr_SuggestInteractionProfileBindings(
        void *instance, const XrInteractionProfileSuggestedBinding *bindings) {
    if (!klxr_inst(instance)) return KLXR_ERROR_HANDLE_INVALID;
    if (!bindings) return KLXR_ERROR_VALIDATION_FAILURE;
    if (bindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
        return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_log_chain("xrSuggestInteractionProfileBindings", bindings->next);

    const char *profile = klxr_path_str(bindings->interactionProfile);
    int active = klxr_profile_driveable(profile);
    int detail = kl_env_on("KL_XR_BINDINGS", 0);
    fprintf(stderr, "  [xr] suggested bindings for %s (%u)%s\n",
            profile, bindings->countSuggestedBindings,
            active ? "  <- driveable (taken)" : "");

    // Only one profile is ever reported active per top-level path, and the
    // last driveable suggestion is the one. Recording it is REQUIRED by the
    // guest, not a nicety: xrGetCurrentInteractionProfile is what VRChat's
    // whole controller surface hangs off, and GetCurrentInteractionProfile
    // cannot stay hardcoded to Touch while the map it must describe is the
    // Valve frame controller's. Wiping the previous driveable profile's map
    // is the same replace rule the spec makes a SECOND call for ONE profile
    // obey: two profiles must not both be reports we answer with.
    if (active) {
        snprintf(g_active_profile, sizeof g_active_profile, "%s", profile);
        // A second call for the same profile REPLACES the first — the spec is
        // explicit, and an app that rebinds mid-session (a settings screen)
        // would otherwise accumulate both maps and read whichever won the
        // last write. Cheap to get right here and impossible to notice later.
        for (int i = 0; i < KLXR_ACTION_MAX; i++)
            if (g_actions[i].magic)
                for (int h = 0; h < 2; h++) {
                    g_actions[i].kind[h] = KLXR_SRC_NONE;
                    g_actions[i].bit[h] = 0;
                    g_actions[i].bind[h] = 0;
                }
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
// Never XR_NULL_PATH, however reasonable "no controller is present, so nothing
// is bound" sounds: the guest keys its whole controller description off this
// value, and nothing bound means SteamVR is never told the controllers exist.
// See the KLXR_ACTIVE_PROFILE comment for why Touch is the consistent answer.
//
// Answered per top-level path, and only for the two hands: a profile for
// `/user/gamepad` would be a claim to have one.
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
        //
        // The answer is the profile the guest actually suggested and we can
        // drive, not a constant: VRChat never suggests Touch, so answering
        // Touch there would describe a controller the guest's whole map is
        // not written for. Steam Link suggests Touch, so it falls back
        // uneventfully. g_active_profile is empty only if nothing driveable
        // was ever suggested, and Touch is then the Quest-shaped answer.
        const char *profile = g_active_profile[0] ? g_active_profile
                                                  : KLXR_ACTIVE_PROFILE;
        klxr_StringToPath(s->instance, profile, &state->interactionProfile);
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
        // isActive from the same field as everything else. STICK_VEC behaves
        // like POSE here too — its real value is a vector and is handled in
        // klxr_SyncActions — and 1 on the way in is only the activity marker.
        case KLXR_SRC_STICK_VEC:
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
            float v = 0.0f, w = 0.0f;
            if (active) {
                if (a->kind[h] == KLXR_SRC_STICK_VEC) {
                    // The whole-stick action: both components, sampled once.
                    // A Vector2f must never be assembled from two reads that
                    // could straddle a sync (the reason nothing here reads
                    // kl_ovrp live), so the vector is captured here the same
                    // way the scalars are.
                    v = in[h].stick_x; w = in[h].stick_y;
                } else {
                    v = klxr_eval(a, h, &in[h]);
                }
            }
            // "Changed" only between two ACTIVE syncs. A controller appearing
            // is not the user pressing anything, and reporting it as a change
            // would fire every edge-triggered handler the guest has the moment
            // a hand comes into view. For a vector, either component moving is
            // a change.
            a->changed[h] = active && a->active[h] &&
                (a->kind[h] == KLXR_SRC_STICK_VEC
                     ? (v != a->value[h] || w != a->value2[h]) : v != a->value[h]);
            if (a->changed[h]) a->change_time[h] = now;
            a->value[h]  = v;
            a->value2[h] = w;
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

// The thumbstick, read as a vector — the action VRChat's `steamframecontroller`
// set calls `thumbstick`, and until 2026-08-14 the getter did not exist at all
// (the entry point resolved to the named-refusal stub), so the guest's whole
// locomotion input read from an action that reported itself active-and-zero.
// The combined-hands rule is the FLOAT rule, not the boolean OR: movement is
// the larger push, so a stick pushed on one hand must not be cancelled by a
// centred one on the other. The value comes from the SNAPSHOT (see the
// struct's comment), never from a live read.
static XrResult klxr_GetActionStateVector2f(void *session,
                                            const XrActionStateGetInfo *info,
                                            XrActionStateVector2f *state) {
    klxr_action *a = NULL;
    XrResult r = klxr_action_state_pre(session, info, "xrGetActionStateVector2f", &a);
    if (r != KLXR_SUCCESS) return r;
    if (!state) return KLXR_ERROR_VALIDATION_FAILURE;
    state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
    state->currentState = (XrVector2f){ 0, 0 };
    state->changedSinceLastSync = 0; state->lastChangeTime = 0; state->isActive = 0;

    int hands = klxr_hands_for(a, info->subactionPath, NULL);
    for (int h = 0; h < 2; h++) {
        if (!(hands & (1 << h)) || !a->active[h]) continue;
        if (!state->isActive) a->active_reads++;
        state->isActive = 1;
        float mx = a->value[h], my = a->value2[h];
        float cm = mx * mx + my * my;
        float pm = state->currentState.x * state->currentState.x +
                   state->currentState.y * state->currentState.y;
        if (cm > pm) {
            state->currentState = (XrVector2f){ mx, my };
            if (cm > 0.0f) klxr_note_active(a, h, sqrtf(cm));
        }
        if (a->changed[h]) state->changedSinceLastSync = 1;
        if (a->change_time[h] > state->lastChangeTime)
            state->lastChangeTime = a->change_time[h];
    }
    if (!state->isActive) {
        state->currentState = (XrVector2f){ 0, 0 };
        state->changedSinceLastSync = 0; state->lastChangeTime = 0;
    }
    return KLXR_SUCCESS;
}

// Which physical paths an action is currently bound to, OUT of the guest and
// back into it. Unity/XR interaction code uses this to build a remapping UI
// and, more importantly, to discover which of an action's potential sources
// are actually live — so an unanswered version is not merely a missing
// convenience, it is one more way the "is this control reachable at all"
// question cannot be asked.
static XrResult klxr_EnumerateBoundSourcesForAction(
        void *session, const XrBoundSourcesForActionEnumerateInfo *info,
        uint32_t sourceCapacityInput, uint32_t *sourceCountOutput,
        XrPath *sources) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !sourceCountOutput) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
        return KLXR_ERROR_VALIDATION_FAILURE;
    klxr_action *a = klxr_action_of(info->action);
    if (!a) return KLXR_ERROR_HANDLE_INVALID;
    if (!s->action_sets_attached) return KLXR_ERROR_ACTIONSET_NOT_ATTACHED;
    klxr_log_chain("xrEnumerateBoundSourcesForAction", info->next);

    // "Bound" is the ACTIVE map, and only that: an action suggested under an
    // inactive profile is not bound to anything, which is exactly what the
    // guest is trying to find out by calling this.
    uint32_t n = 0;
    XrPath all[2];
    for (int h = 0; h < 2; h++)
        if (a->kind[h] != KLXR_SRC_NONE && a->bind[h]) all[n++] = a->bind[h];
    *sourceCountOutput = n;
    if (sourceCapacityInput == 0) return KLXR_SUCCESS;   // the two-call shape
    if (sourceCapacityInput < n) return KLXR_ERROR_SIZE_INSUFFICIENT;
    if (n && !sources) return KLXR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < n; i++) sources[i] = all[i];
    return KLXR_SUCCESS;
}

// A display name for a source path — the input catalogue, spelled for a
// person. The profile arm is answered only as a name we can stand behind, and
// the component arm by the last path element, because that is the only
// component granularity we actually have. A name is never load-bearing; the
// `whichComponents` bits only say how much of one to build.
static const char *klxr_src_display_name(const char *path) {
    const char *hand = strstr(path, "/user/hand/");
    const char *hand_name = "Controller";
    if (hand) {
        const char *base = hand + strlen("/user/hand/");
        const char *sl = strchr(base, '/');
        size_t n = sl ? (size_t)(sl - base) : strlen(base);
        if (n == 4 && strncmp(base, "left", 4) == 0)
            hand_name = "Left Controller";
        else if (n == 5 && strncmp(base, "right", 5) == 0)
            hand_name = "Right Controller";
    }
    static __thread char buf[128];
    const char *comp = strrchr(path, '/');
    const char *comp_name = comp ? comp + 1 : path;
    snprintf(buf, sizeof buf, "%s — %s", hand_name, comp_name);
    return buf;
}

static XrResult klxr_GetInputSourceLocalizedName(
        void *session, const XrInputSourceLocalizedNameGetInfo *info,
        uint32_t bufferCapacityInput, uint32_t *bufferCountOutput,
        char *buffer) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    if (!info || !bufferCountOutput) return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
        return KLXR_ERROR_VALIDATION_FAILURE;
    if (info->sourcePath == 0 || info->sourcePath > g_path_count)
        return KLXR_ERROR_PATH_INVALID;
    klxr_log_chain("xrGetInputSourceLocalizedName", info->next);

    const char *name = klxr_src_display_name(klxr_path_str(info->sourcePath));
    uint32_t need = (uint32_t)strlen(name) + 1;  // count includes the NUL
    *bufferCountOutput = need;
    if (bufferCapacityInput < need) return KLXR_ERROR_SIZE_INSUFFICIENT;
    if (!buffer) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(buffer, name, need);
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
            g_active_profile[0] ? g_active_profile : KLXR_ACTIVE_PROFILE);
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
// The eye seam runs the OPPOSITE direction from OVRPlugin's: in OpenXR the
// RUNTIME owns the eye images and lends
// them to the app, where Unity generates the texture names and
// ovrp_SetupEyeTexture2 is handed one to put storage behind. The guest
// therefore renders into textures created here, so the compositor's seam needs
// no recovery from a name the guest picked.
//
// An image is created exactly as kl_ovrp's eye textures are and registered with
// kl_glfb the same way — kl_glfb_note_eye_texture for the capture path, plus the
// MTLTexture provider when one is present — so these images ARE the MTLTextures
// KleptonCompositor samples. The visionOS side is the identical seam reached
// through a different API.
//
// Deliberately absent: multisampling. sampleCount > 1 needs an MSAA texture and
// a resolve, and the VRR work keeps the eye resolve a
// physical-to-physical copy. The view configuration recommends 1 sample; an app
// asking for more gets a named refusal rather than a silent downgrade, which
// would leave it believing its edges were antialiased.
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
    uint32_t tex[KLXR_SWAPCHAIN_IMAGES];       // GLES: texture names
    uint64_t vk_img[KLXR_SWAPCHAIN_IMAGES];    // Vulkan: VkImage handles
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
// with.
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

// ...and the same list in Vulkan's vocabulary, because a swapchain format is
// stated in the vocabulary of the session's graphics API and nothing converts
// between them. **This is what Open Brush's first Vulkan session died on**: it
// enumerated formats, got GL internal formats (0x8C43 and friends), recognised
// none of them as a VkFormat, and reported "Failed to create swapchain, exiting
// session" — a correct app refusing a runtime that answered in the wrong units.
//
// The numbers are VkFormat enumerants, written out rather than included: this
// file must not pull in a Vulkan header (see the include block). They are
// checked against <vulkan/vulkan_core.h> by `make vkabi`'s sibling assertion in
// kl_vulkan.c — a wrong number here is a silently different format, not an error.
#define KLXR_VK_FORMAT_R8G8B8A8_SRGB       43
#define KLXR_VK_FORMAT_R8G8B8A8_UNORM      37
#define KLXR_VK_FORMAT_R16G16B16A16_SFLOAT 97
#define KLXR_VK_FORMAT_D24_UNORM_S8_UINT  129
#define KLXR_VK_FORMAT_D32_SFLOAT         126
#define KLXR_VK_FORMAT_D16_UNORM          124
static const int64_t g_swapchain_formats_vk[] = {
    KLXR_VK_FORMAT_R8G8B8A8_SRGB, KLXR_VK_FORMAT_R8G8B8A8_UNORM,
    KLXR_VK_FORMAT_R16G16B16A16_SFLOAT,
    KLXR_VK_FORMAT_D24_UNORM_S8_UINT, KLXR_VK_FORMAT_D32_SFLOAT,
    KLXR_VK_FORMAT_D16_UNORM,
};
#define KLXR_FORMAT_COUNT_VK \
    ((uint32_t)(sizeof g_swapchain_formats_vk / sizeof g_swapchain_formats_vk[0]))

// Which list a session speaks. One function, so the enumerate and the
// create-time validation cannot disagree about what was offered — which is the
// same failure the extension list next door was restructured to prevent.
static const int64_t *klxr_formats_for(const klxr_session *s, uint32_t *count_out) {
    if (s && s->gfx == KLXR_GFX_VULKAN) {
        if (count_out) *count_out = KLXR_FORMAT_COUNT_VK;
        return g_swapchain_formats_vk;
    }
    if (count_out) *count_out = KLXR_FORMAT_COUNT;
    return g_swapchain_formats;
}

static int klxr_format_is_depth(const klxr_session *s, int64_t f) {
    if (s && s->gfx == KLXR_GFX_VULKAN)
        return f == KLXR_VK_FORMAT_D24_UNORM_S8_UINT ||
               f == KLXR_VK_FORMAT_D32_SFLOAT ||
               f == KLXR_VK_FORMAT_D16_UNORM;
    return f == KLXR_GL_DEPTH_COMPONENT24 || f == KLXR_GL_DEPTH_COMPONENT16 ||
           f == KLXR_GL_DEPTH24_STENCIL8;
}

static XrResult klxr_EnumerateSwapchainFormats(void *session, uint32_t capacity,
                                               uint32_t *count_out, int64_t *formats) {
    klxr_session *s = klxr_sess(session);
    if (!s) return KLXR_ERROR_HANDLE_INVALID;
    uint32_t have = 0;
    const int64_t *list = klxr_formats_for(s, &have);
    XrResult r = klxr_two_call(capacity, count_out, have);
    if (r != KLXR_SUCCESS || capacity == 0) return r;
    if (!formats) return KLXR_ERROR_VALIDATION_FAILURE;
    memcpy(formats, list, have * sizeof *list);
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

    uint32_t nfmt = 0;
    const int64_t *fmts = klxr_formats_for(s, &nfmt);
    int known = 0;
    for (uint32_t i = 0; i < nfmt; i++)
        if (fmts[i] == info->format) known = 1;
    if (!known) {
        fprintf(stderr, "  [xr] xrCreateSwapchain: format 0x%llx unsupported (%s)\n",
                (unsigned long long)info->format,
                s->gfx == KLXR_GFX_VULKAN ? "VkFormat" : "GL internal format");
        return KLXR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    if (info->sampleCount > 1) {
        fprintf(stderr, "  [xr] xrCreateSwapchain: sampleCount %u — only 1 is served\n",
                info->sampleCount);
        return KLXR_ERROR_FEATURE_UNSUPPORTED;
    }
    // faceCount 6 is a cubemap. Nothing in the eye path wants one; refused
    // rather than served as a 2D image.
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

    if (s->gfx == KLXR_GFX_VULKAN) {
        // The images are VkImages, allocated by kl_vulkan on the device the
        // guest itself created. Nothing GL is touched — and it must not be:
        // there is no EGL context on this path at all, so the GL gateway below
        // would resolve nothing.
        for (int i = 0; i < sc->count; i++) {
            sc->vk_img[i] = kl_vulkan_xr_image(sc->width, sc->height,
                                               sc->array_size, sc->mip_count,
                                               (long long)sc->format,
                                               klxr_format_is_depth(s, sc->format));
            if (!sc->vk_img[i]) {
                fprintf(stderr, "  [xr] xrCreateSwapchain: VkImage %d of %d failed\n",
                        i, sc->count);
                sc->magic = 0;
                return KLXR_ERROR_RUNTIME_FAILURE;
            }
        }
        fprintf(stderr, "  [xr] swapchain %ux%u VkFormat %lld array %u mips %u "
                        "usage 0x%llx -> %d VkImage(s)%s\n",
                sc->width, sc->height, (long long)sc->format,
                sc->array_size, sc->mip_count, (unsigned long long)sc->usage,
                sc->count, klxr_format_is_depth(s, sc->format) ? " [depth]" : "");
        *swapchain = sc;
        return KLXR_SUCCESS;
    }

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
            klxr_format_is_depth(sc->session, sc->format) ? " [depth]" : "");
    *swapchain = sc;
    return KLXR_SUCCESS;
}

// ---- giving an eye swapchain's images MTLTexture storage -----------------
//
// The ordering problem, and why this is here and not in xrCreateSwapchain: the
// provider is asked for storage for a texture the guest has already told us
// is an eye. An OpenXR guest cannot tell us that at allocation time:
// xrCreateSwapchain knows a size and a format, nothing more, and *which*
// swapchain is an eye is only asserted at xrEndFrame, from the projection
// layer. Creation order does not say it either, measured and wrong: this guest
// makes a pair per projection layer, more for its UI panels and its
// video stream, and rebuilds all of them across scene changes.
//
// So the backing is retroactive: the guest renders into ordinary GL storage
// until it presents a frame that names the swapchain as an eye, and from that
// frame on the same GL texture names are re-pointed at MTLTextures the
// compositor owns. ONE frame per swapchain generation is lost — drawn into
// storage nothing samples — and that is the whole price. The
// alternative, backing every swapchain image at creation, costs the memory of
// every UI panel and stream buffer as well; this guest submits four projection
// layers a frame plus 1536x1536 panels, so it is not a small difference.
//
// Re-pointing a texture that already has immutable glTexStorage2D storage is
// legal: glEGLImageTargetTexture2DOES respecifies the texture, ANGLE's
// validation has no immutability test and Texture::setEGLImageTargetImpl
// orphans the previous storage. `make mtltex` checks it against real ANGLE
// rather than against that reading.
// The Vulkan eye seam, and it runs the OPPOSITE DIRECTION to both GL routes.
//
// On GL the guest rendered into storage of its own and the eye is delivered by
// re-pointing that storage at an MTLTexture the provider owns (an import), or —
// when the swapchain is an array and re-pointing is impossible — by copying into
// one. Under Vulkan there is nothing to provide and nothing to copy: the image
// was allocated through MoltenVK, which has ALREADY backed it with an
// MTLTexture, so the texture the compositor wants exists before we ask. We
// EXPORT it. That is BONELAB's finding reached through the
// other API, and it is why this path needs no provider at all — which matters,
// because a host run without a frontend has none.
//
// Every image of the swapchain becomes a STAGE, because the guest rotates
// through them and the frame record names the one it drew. Publishing only the
// released image would leave the compositor sampling a stage that was never
// filled on the two frames either side of it.
static void klxr_publish_eye_vulkan(klxr_swapchain *sc, int eye, int slice) {
    if (sc->mtl_eye == eye) return;
    if (klxr_format_is_depth(sc->session, sc->format)) return;
    int done = 0;
    for (int k = 0; k < sc->count; k++) {
        void *tex = kl_vulkan_xr_image_mtl(sc->vk_img[k]);
        if (!tex) continue;
        kl_glfb_note_eye_mtl_texture(eye, k, tex, slice,
                                     (int)sc->width, (int)sc->height);
        done++;
    }
    if (done == sc->count) {
        // Same rule as the GL route: exactly one swapchain owns an eye at a
        // time, and the guest rebuilds its eye swapchains across scenes with
        // the OLD one destroyed after the new one is asserted.
        for (int i = 0; i < KLXR_SWAPCHAIN_MAX; i++)
            if (&g_swapchains[i] != sc && g_swapchains[i].mtl_eye == eye)
                g_swapchains[i].mtl_eye = -1;
        sc->mtl_eye = eye;
        fprintf(stderr, "  [xr] eye %d: %d Vulkan swapchain image(s) %ux%u slice %d "
                        "exported as MTLTextures — the compositor can sample them\n",
                eye, done, sc->width, sc->height, slice);
    } else {
        // Named rather than silent, and not fatal: the guest keeps rendering
        // into images that are simply never sampled, which is a black eye with
        // every counter healthy — the exact failure `e0=nil e1=nil` could not
        // explain on the GL path.
        fprintf(stderr, "  [xr] eye %d: only %d of %d Vulkan images could be exported "
                        "(%ux%u) — no MTLTexture, so the compositor shows black "
                        "for this eye\n",
                eye, done, sc->count, sc->width, sc->height);
    }
}

static void klxr_back_eye_images(klxr_swapchain *sc, int eye) {
    if (!kl_glfb_has_mtl_provider() || sc->mtl_eye == eye) return;
    if (klxr_format_is_depth(sc->session, sc->format)) return;
    // The provider hands back a slice of a 2-slice array, one per eye, and the
    // compositor's amplified pass depends on both eyes sharing that texture. A
    // swapchain that is itself an array is a different shape — the guest renders
    // BOTH eyes into one texture of ours, and the re-point below cannot serve it
    // at all, because glEGLImageTargetTexture2DOES only takes a GL_TEXTURE_2D.
    // klxr_mirror_eye_image is that case and the caller routes it there; this
    // one is unreachable and stays as the statement of what is not served here.
    if (sc->array_size != 1 || sc->mip_count != 1) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [xr] eye %d swapchain is %u array slice(s) x %u mip(s) "
                            "— not backed by an MTLTexture directly\n",
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

// ...and the array case, which is every Unity OpenXR guest: one swapchain, two
// eyes, told apart by `imageArrayIndex`. There is nothing to re-point, so the
// presented layer is copied into storage the compositor owns — see
// kl_glfb_mirror_eye_layer for why a copy and what it costs.
//
// Unlike the re-point this is per FRAME, not once per swapchain: the guest keeps
// its own images and keeps rotating them, and each frame's picture has to be
// carried across. The stage is the image the guest just presented, so the
// destinations rotate exactly as its images do — and it is the same number
// `drawn_stage` files the frame record under, which is what keeps the pose and
// the picture the compositor pairs belonging to one frame.
// KL_XR_EYE_MIRROR: copy the guest's presented eye image (1, the default) or
// re-point its own swapchain texture at MTLTexture storage (0).
//
// **The default moved to the copy on 2026-08-16, on a measurement.** The
// re-point is what klxr_back_eye_images does and it BREAKS JKXR's rendering:
// the same scene, the same frame, with the provider registered comes out
// 2875392 of 5496000 lit with the top row and the right-hand column of the
// picture missing, and with it not registered comes out 5496000 of 5496000 and
// correct. That guest attaches its swapchain images to framebuffers it builds
// once and keeps; replacing the storage under a live attachment is not
// something ES promises anything about, and nothing visible from here
// distinguishes a guest that does it from one that does not. The copy cannot
// fail that way — the destination is a texture of ours nothing has attached —
// and it is the same blit every array-swapchain guest has always paid.
//
// 0 restores the re-point, which is the A/B for a target where the copy's
// per-frame cost matters (Steam Link streams video and is the one measured on
// device). An ARRAY swapchain cannot be re-pointed at all, so there 0 means no
// eye texture and a black display, which is the state this knob's old name
// described.
// Defined below, beside the quad path that also uses it: a swapchain's index in
// the table, which is the stable per-swapchain identity the eye mirror keys on.
static int klxr_layer_id(const klxr_swapchain *sc);

static int klxr_eye_mirror_on(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_XR_EYE_MIRROR", 1);
        if (!on)
            fprintf(stderr, "  [xr] KL_XR_EYE_MIRROR=0 — the guest's own swapchain "
                            "textures are re-pointed at MTLTextures instead of "
                            "being copied\n");
    }
    return on;
}

// Returns the compositor SLOT the picture landed in, or -1. That number, not
// the swapchain's own image index, is what a compositor reads the eye back with
// — see kl_glfb_mirror_eye_layer, and note that the frame record's `stage` is
// therefore this and not `sc->last_released`.
static int klxr_mirror_eye_image(klxr_swapchain *sc, int eye, int layer) {
    if (!kl_glfb_has_mtl_provider()) return -1;
    if (klxr_format_is_depth(sc->session, sc->format)) return -1;
    if (sc->last_released < 0 || sc->last_released >= sc->count) return -1;
    if (sc->mip_count != 1) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [xr] eye %d swapchain has %u mips — only level 0 is "
                            "copied to the compositor\n", eye, sc->mip_count);
    }
    if (layer < 0 || (uint32_t)layer >= sc->array_size) layer = 0;
    // +1 so swapchain 0 is not the "unknown source" sentinel.
    return kl_glfb_mirror_eye_layer(eye, sc->last_released,
                                    (uint32_t)(klxr_layer_id(sc) + 1),
                                    sc->tex[sc->last_released],
                                    sc->array_size > 1 ? layer : -1,
                                    (int)sc->width, (int)sc->height,
                                    (uint32_t)sc->format);
}

// ---------------------------------------------------------------------------
// The QUAD layer — a flat panel with a pose and a size in metres, which for
// JKXR is the entire frame: 7381 of 7381 layers in a host run, one per frame,
// 6.00 x 5.50 m at (0, 1, -5.5) in STAGE space, both eyes from one image. An
// engine in "cinematic" mode presents its whole picture this way and a
// projection layer never appears at all.
//
// It is composited through the OVERLAY pass (kl_reproject.h), which already
// existed for RE4's UI quads and wants exactly this: a pose in the tracking
// space, a size in metres, a texture rect and an MTLTexture. So the work here is
// the two things OpenXR states that OVRPlugin does not — which space the pose is
// in, and which eyes the layer is for — plus giving the guest's swapchain images
// storage a compositor can sample.
//
// A layer's identity, for the record and for the texture table, is its
// SWAPCHAIN's slot. The submission has no id of its own, the slot is unique
// while the swapchain lives, and a guest that moves a panel keeps its swapchain.
static int klxr_layer_id(const klxr_swapchain *sc) {
    return (int)(sc - g_swapchains);
}

// Storage for a quad's image, one route per graphics API, both ending in
// kl_glfb's layer table. Vulkan's image already IS an MTLTexture and is only
// recorded; GL has to be COPIED into storage of ours — see kl_glfb_mirror_layer
// for why a copy and not the eye path's re-point, which broke this guest's
// rendering outright.
//
// The Vulkan half publishes every image of the swapchain, because the guest
// rotates through them and each one is its own MTLTexture. The GL half copies
// only the image the guest just presented, per frame, into the destination that
// image's stage owns — so the destinations rotate exactly as the guest's own
// images do, which is what keeps the compositor from sampling a stage the guest
// is currently drawing into.
static void klxr_back_layer_images(klxr_swapchain *sc, int slice) {
    int id = klxr_layer_id(sc);
    if (sc->session && sc->session->gfx == KLXR_GFX_VULKAN) {
        for (int k = 0; k < sc->count; k++) {
            void *tex = kl_vulkan_xr_image_mtl(sc->vk_img[k]);
            if (tex) kl_glfb_note_layer_mtl_texture(id, k, tex,
                                                    (int)sc->width, (int)sc->height);
        }
        return;
    }
    if (!kl_glfb_has_mtl_layer_provider()) {
        // Named once, and not fatal: the guest keeps rendering into its own GL
        // storage and the panel simply does not reach the display. A run with no
        // frontend (make check, a headless host run) is the ordinary case for
        // this, which is why it says which half is missing rather than sounding
        // like a failure.
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [xr] no Metal layer provider — the guest's quad "
                            "layer(s) are captured but not composited (a host run "
                            "without KL_VIEW has no frontend to allocate them)\n");
        }
        return;
    }
    if (sc->mip_count != 1) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [xr] quad swapchain has %u mips — only level 0 is "
                            "copied to the compositor\n", sc->mip_count);
    }
    kl_glfb_mirror_layer(id, sc->last_released, sc->tex[sc->last_released],
                         sc->array_size > 1 ? slice : -1,
                         (int)sc->width, (int)sc->height, (uint32_t)sc->format);
}

// One quad, as the record both compositors read. Returns 0 for a submission
// that cannot be placed, which is then counted as ignored rather than drawn
// somewhere invented.
static int klxr_quad_record(const XrCompositionLayerQuad *q, klxr_swapchain *sc,
                            kl_ovrp_overlay *o) {
    if (!(q->size.width > 0.0f) || !(q->size.height > 0.0f)) return 0;
    memset(o, 0, sizeof *o);
    o->layer_id = klxr_layer_id(sc);
    o->shape    = 0;                          // ovrpShape Quad, the record's vocabulary
    o->stage    = sc->last_released;
    o->tex_w    = (int)sc->width;
    o->tex_h    = (int)sc->height;
    // The rect is stated in the guest's OWN framebuffer convention — the origin
    // is the lower left for a GL swapchain and the upper left for a Vulkan one —
    // and so is the v axis of the texture it indexes. The two therefore agree
    // without a mirror on either path, and `origin_top_left` below is the single
    // place the difference is expressed. (Measured only on a full-image rect;
    // JKXR submits the whole image every frame.)
    const XrRect2Di *r = &q->subImage.imageRect;
    for (int e = 0; e < 2; e++) {
        o->viewport[e][0] = r->offset.x;
        o->viewport[e][1] = r->offset.y;
        o->viewport[e][2] = r->extent.width;
        o->viewport[e][3] = r->extent.height;
    }
    // The layer states its pose in ITS space, which need not be the tracking
    // space, so it is composed out of that one exactly as a projection layer's
    // views are. A quad in VIEW space comes out head-locked at the pose the
    // frame was latched with, which is what that space MEANS.
    klxr_space *lsp = klxr_space_of(((const XrCompositionLayerBaseHeader *)q)->space);
    XrPosef in_tracking = klxr_pose_apply(
        lsp ? klxr_space_pose(lsp) : (XrPosef){{0,0,0,1},{0,0,0}}, q->pose);
    o->pose[0] = in_tracking.orientation.x;
    o->pose[1] = in_tracking.orientation.y;
    o->pose[2] = in_tracking.orientation.z;
    o->pose[3] = in_tracking.orientation.w;
    o->pose[4] = in_tracking.position.x;
    o->pose[5] = in_tracking.position.y;
    o->pose[6] = in_tracking.position.z;
    o->size[0] = q->size.width;
    o->size[1] = q->size.height;
    o->flags   = (int)q->layerFlags;
    // Already resolved into the tracking space above, so the compositor must
    // NOT apply the head pose a second time. Head-locking is a property of the
    // SPACE in OpenXR, not of a flag.
    o->head_locked = 0;
    o->eye_visibility = q->eyeVisibility;
    o->origin_top_left = sc->session && sc->session->gfx == KLXR_GFX_VULKAN;
    return 1;
}

// The SHAPE of a frame's submission — how many layers, of which types, naming
// which swapchains — printed when it changes and not otherwise.
//
// This is the question no other line here answers. "This guest submits one quad
// and nothing else" was measured over 5000 frames of the intro and the menu and
// is FALSE somewhere else in the run: a projection layer appearing for one scene
// puts the eye pass on screen underneath the panel, and from the compositor's
// side that is indistinguishable from a bug in the panel. A frame shape that
// changes at a scene transition says which of the two is happening, in one line,
// on the run where it happens.
// Which (layer, eye) slots the geometry census has already described. Re-armed
// by klxr_frame_shape on every shape change — see there.
static uint16_t g_geo_said;
// The same re-arm covers the foveal placement line below, which is a per-shape
// fact for the same reason.
static uint8_t g_fovea_said;
static void klxr_geo_census_rearm(void) { g_geo_said = 0; g_fovea_said = 0; }

static void klxr_frame_shape(const XrFrameEndInfo *info) {
    char shape[256];
    int n = 0;
    for (uint32_t i = 0; i < info->layerCount && n < (int)sizeof shape - 32; i++) {
        const XrCompositionLayerBaseHeader *l = info->layers[i];
        if (!l) { n += snprintf(shape + n, sizeof shape - (size_t)n, " null"); continue; }
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p =
                (const XrCompositionLayerProjection *)l;
            n += snprintf(shape + n, sizeof shape - (size_t)n, " proj(%u views:",
                          p->viewCount);
            for (uint32_t v = 0; v < p->viewCount && v < 2; v++) {
                klxr_swapchain *sc = klxr_swapchain_of(p->views[v].subImage.swapchain);
                n += snprintf(shape + n, sizeof shape - (size_t)n, " sc%d",
                              sc ? klxr_layer_id(sc) : -1);
            }
            n += snprintf(shape + n, sizeof shape - (size_t)n, ")");
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
            klxr_swapchain *sc = klxr_swapchain_of(q->subImage.swapchain);
            n += snprintf(shape + n, sizeof shape - (size_t)n, " quad(sc%d eyes%d)",
                          sc ? klxr_layer_id(sc) : -1, q->eyeVisibility);
        } else {
            n += snprintf(shape + n, sizeof shape - (size_t)n, " type%d", l->type);
        }
    }
    static char last[256];
    static uint64_t since;
    since++;
    if (strcmp(shape, last) == 0) return;
    // A new shape is a new set of swapchains, so every per-layer fact measured
    // against the old one describes layers that are gone. Re-arm the geometry
    // census: without this it fires once, on whatever the guest submitted
    // FIRST, which for a streaming client is its loading scene — three
    // identical full-FOV layers, measured, and nothing like the stream.
    klxr_geo_census_rearm();
    if (last[0])
        fprintf(stderr, "  [xr] frame shape changed after %llu frames:%s ->%s\n",
                (unsigned long long)since, last, shape);
    else
        fprintf(stderr, "  [xr] frame shape:%s\n", shape);
    snprintf(last, sizeof last, "%s", shape);
    since = 0;
}

// What is being composited, said when it CHANGES rather than per frame: a
// static panel costs one line for the run and a menu that moves says so. Same
// argument as klovrp_census_submits, which is the OVRPlugin half of this.
static void klxr_quad_census(const kl_ovrp_overlay *o, int placed) {
    static kl_ovrp_overlay last[4];
    static int last_placed[4];
    // Everything but the STAGE, which is the swapchain image the guest just
    // released and therefore changes every frame — a census keyed on it is a
    // per-frame trace. It is still printed, because which image the first frame
    // landed in is worth one line.
    kl_ovrp_overlay key = *o;
    key.stage = 0;
    int k = o->layer_id & 3;
    if (last_placed[k] == placed && memcmp(&last[k], &key, sizeof key) == 0) return;
    last[k] = key;
    last_placed[k] = placed;
    fprintf(stderr, "  [xr] quad layer %d stage %d: %.2fx%.2f m at (%.2f %.2f %.2f) "
                    "in the tracking space, %ux%u image rect %dx%d+%d+%d, eyes %s, "
                    "origin %s, flags 0x%x — %s\n",
            o->layer_id, o->stage, (double)o->size[0], (double)o->size[1],
            (double)o->pose[4], (double)o->pose[5], (double)o->pose[6],
            (unsigned)o->tex_w, (unsigned)o->tex_h,
            o->viewport[0][2], o->viewport[0][3], o->viewport[0][0], o->viewport[0][1],
            o->eye_visibility == 1 ? "LEFT" : o->eye_visibility == 2 ? "RIGHT" : "both",
            o->origin_top_left ? "top left" : "bottom left", (unsigned)o->flags,
            placed ? "composited" : "NOT composited (no MTLTexture for it)");
}

static XrResult klxr_DestroySwapchain(void *swapchain) {
    klxr_swapchain *sc = klxr_swapchain_of(swapchain);
    if (!sc) return KLXR_ERROR_HANDLE_INVALID;
    // The compositor slots this swapchain's images were copied into, which are
    // ours and are keyed on the swapchain rather than on its image index — so
    // they cannot be found from `count` the way the eye textures below are.
    // Same +1 as the mirror uses, so swapchain 0 is not the unknown sentinel.
    kl_glfb_forget_eye_source((uint32_t)(klxr_layer_id(sc) + 1));
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
    // The element type is checked against the SESSION's binding rather than
    // simply accepted, because the two ways this can be wrong are opposite: an
    // app whose array does not match its own binding is confused, and a runtime
    // that writes the wrong width into it is catastrophic and silent (a VkImage
    // truncated into a uint32 is a plausible-looking GL name).
    int32_t got = *(const int32_t *)images;
    if (sc->session && sc->session->gfx == KLXR_GFX_VULKAN) {
        if (got != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
            fprintf(stderr, "  [xr] xrEnumerateSwapchainImages: image type %d is not "
                            "XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR\n", got);
            return KLXR_ERROR_VALIDATION_FAILURE;
        }
        XrSwapchainImageVulkanKHR *vk = (XrSwapchainImageVulkanKHR *)images;
        for (int i = 0; i < sc->count; i++) vk[i].image = sc->vk_img[i];
        return KLXR_SUCCESS;
    }
    if (got != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
        fprintf(stderr, "  [xr] xrEnumerateSwapchainImages: image type %d is not "
                        "XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR\n", got);
        return KLXR_ERROR_VALIDATION_FAILURE;
    }
    XrSwapchainImageOpenGLESKHR *gl = (XrSwapchainImageOpenGLESKHR *)images;
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
// head turns and grows as the frame rate falls. OpenXR's frame loop
// has exactly one place that means "the guest's next frame starts here", and
// this is it.
// See kl_openxr.h. NULL is the command line's state and the default.
static void (*g_frame_pacer)(void);
void kl_openxr_set_frame_pacer(void (*wait)(void)) { g_frame_pacer = wait; }

// Set by a driver whose guest stacks projection layers (Steam Link): capture
// the topmost layer rather than layer 0. See kl_openxr.h. Read in xrEndFrame.
static int g_capture_topmost_layer;
void kl_openxr_set_capture_topmost_layer(int on) { g_capture_topmost_layer = on ? 1 : 0; }

// ---- the foveal inset's stability, once a second (KL_XR_FOVEA_TRACE)
//
// Two different defects present as the same thing to the eye — the inset
// dropping out on some frames, and its placement wandering while it stays — and
// both read as the picture stepping about a frame at a time. Neither is visible
// in any per-frame number, so this accumulates over a second and prints what
// separates them: how many submissions laid the inset down, how many did not
// and for what reason, and how far the computed rect travelled while they did.
//
// Placement is reported in THOUSANDTHS of the eye, which is KL_XR_FOVEA_SHIFT_Y's
// unit on purpose: a y span of 4 here is a defect a shift of 4 would move.
static void klxr_fovea_trace(int eye, float x0, float y0, float x1, float y1,
                             const char *skip) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_XR_FOVEA_TRACE", 0);
    if (!on || eye < 0 || eye > 1) return;
    static struct {
        int64_t     t0;
        unsigned    cands, drawn;
        unsigned    skipped;
        const char *why;              // the last skip reason seen this second
        float       xlo, xhi, ylo, yhi;
    } fv[2];
    int64_t now = klxr_now();
    if (!fv[eye].t0) fv[eye].t0 = now;
    fv[eye].cands++;
    if (skip) {
        fv[eye].skipped++;
        fv[eye].why = skip;
    } else {
        if (!fv[eye].drawn++) {
            fv[eye].xlo = fv[eye].xhi = x0;
            fv[eye].ylo = fv[eye].yhi = y0;
        }
        if (x0 < fv[eye].xlo) fv[eye].xlo = x0;
        if (x0 > fv[eye].xhi) fv[eye].xhi = x0;
        if (y0 < fv[eye].ylo) fv[eye].ylo = y0;
        if (y0 > fv[eye].yhi) fv[eye].yhi = y0;
    }
    if (now - fv[eye].t0 < 1000000000LL) return;
    fprintf(stderr,
        // "candidates", not "frames": a guest stacking three projection layers
        // offers two of them per frame as inset candidates, so this counts
        // opportunities rather than frames and the ratio is what matters.
        "  [xr] fovea eye %d: %u candidate(s), %u laid down, %u skipped%s%s; "
        "placement travel x %.1f y %.1f (thousandths of the eye)\n",
        eye, fv[eye].cands, fv[eye].drawn, fv[eye].skipped,
        fv[eye].skipped ? " — " : "", fv[eye].skipped ? fv[eye].why : "",
        (double)((fv[eye].xhi - fv[eye].xlo) * 1000.0f),
        (double)((fv[eye].yhi - fv[eye].ylo) * 1000.0f));
    memset(&fv[eye], 0, sizeof fv[eye]);
}

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
    // the temporal doubling the latch exists to prevent.
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

    // Which projection layer the capture reads. Some guests submit more than
    // one — Steam Link submits several, each with a left and a right view — and
    // only one of them holds the streamed picture. Nothing in a single layer's
    // submission says which, but their ORDER does: OpenXR draws projection
    // layers back-to-front, so layer 0 is the backdrop (Steam Link's room,
    // measured black in an empty space) and the LAST one is the picture on top.
    //
    // Three ways to pick, in priority order:
    //   1. KL_XR_CAPTURE_LAYER=N pins index N (the debug override, and the way a
    //      run that read the wrong one is retried without a rebuild — a miss
    //      costs a fresh Steam pairing).
    //   2. A driver that knows its guest stacks layers
    //      (kl_openxr_set_capture_topmost_layer) → the topmost submitted this
    //      frame, computed below once the count is known.
    //   3. Otherwise layer 0, which is every single-layer guest and the old
    //      behaviour unchanged.
    //
    // This is also which layer the COMPOSITOR shows: it draws one quad per eye
    // out of one array texture, so it can show exactly one projection layer.
    // Layering the rest (their own quads, depths, submission order) is real work
    // for later; until then the topmost is the closest single-layer answer.
    int env_cap = kl_env_int("KL_XR_CAPTURE_LAYER", -1);
    // Pre-pass over the projection layers, so "which one" has an answer before
    // the loop that walks them. Two facts per layer, by its running index:
    // whether it exists at all, and its horizontal field of view — because
    // "topmost" alone picked the wrong one. Steam Link stacks, per eye, a
    // full-FOV eye view (~105°, the immersive picture) AND a narrow ~60° panel
    // (the flat "screen" / HUD) composited on top. The literal topmost is that
    // narrow panel — a small square that follows the head — so topmost has to
    // mean "topmost of the layers that fill the eye", skipping the panels.
    uint32_t proj_layer_count = 0;
    float    max_span = 0.0f;
    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = info->layers[i];
        if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        proj_layer_count++;
        const XrCompositionLayerProjection *p =
            (const XrCompositionLayerProjection *)l;
        if (p->viewCount >= 1) {
            float span = fabsf(p->views[0].fov.angleLeft) +
                         fabsf(p->views[0].fov.angleRight);
            if (span > max_span) max_span = span;
        }
    }
    int cap_layer;
    if (env_cap >= 0) {
        cap_layer = env_cap;                       // pinned override
    } else if (g_capture_topmost_layer && proj_layer_count > 0) {
        // The WIDEST layer, not the topmost wide-ish one, and the reason is
        // measured. "Topmost within 80% of the widest" admits a foveal inset:
        // Steam Link's is 88% of the base's span, and on the frames where it
        // submits the base and the inset WITHOUT its high-resolution third
        // layer, the inset is both within the threshold and topmost — so the
        // narrow centre became the eye. The compositor then placed a full eye
        // with the inset's frustum (`tan t0.795 b0.994` against the base's
        // `t1.001 b1.193`), which is a vertical shift of the whole picture,
        // appearing and disappearing as the guest's layer count changes ~once a
        // second. It never reproduced on the host, whose layer shape is stable.
        //
        // Widest is the definition of "fills the eye" and needs no threshold.
        // Ties are exact here — Steam Link's low- and high-resolution base
        // layers state the same frustum — so they break on the larger image
        // (more detail for the same field of view) and then on the topmost, in
        // that order. Keeping the eye on one layer across a layer-count change
        // is what stops the source flipping under the mirror at all.
        cap_layer = 0;
        int   j = 0;
        float best_span = -1.0f;
        long  best_area = -1;
        for (uint32_t i = 0; i < info->layerCount; i++) {
            const XrCompositionLayerBaseHeader *l = info->layers[i];
            if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const XrCompositionLayerProjection *p =
                (const XrCompositionLayerProjection *)l;
            float span = p->viewCount >= 1
                ? fabsf(p->views[0].fov.angleLeft) +
                  fabsf(p->views[0].fov.angleRight)
                : 0.0f;
            long area = 0;
            if (p->viewCount >= 1) {
                klxr_swapchain *csc =
                    klxr_swapchain_of(p->views[0].subImage.swapchain);
                if (csc) area = (long)csc->width * (long)csc->height;
            }
            // A hair of tolerance, because these are tangents of angles the
            // guest recomputes per frame: two layers meaning the same frustum
            // must not alternate on the last bit of a float.
            if (span > best_span * 1.001f ||
                (span >= best_span * 0.999f && area >= best_area)) {
                if (span > best_span) best_span = span;
                best_area = area;
                cap_layer = j;
            }
            j++;
        }
    } else {
        cap_layer = 0;                             // base layer / single-layer guests
    }
    uint32_t proj_layers = 0;
    int drawn_stage = -1;
    // Foveal-inset composite (KL_XR_FOVEA, on by default with the layer-stacking
    // guests). Steam Link streams a wide low-detail base layer plus a narrow
    // high-detail centre inset; cap_layer captures the base, and these remember,
    // per eye, the base's stage and its signed FOV tangents so a later narrow
    // layer can be laid over the centre where its own frustum falls. Set as the
    // base layer's views are walked, read as the inset's are. KL_XR_FOVEA_FLIP
    // inverts the vertical mapping if the guest's image rows run bottom-up.
    static int fovea_on = -1, fovea_flip = -1;
    if (fovea_on < 0)   fovea_on   = g_capture_topmost_layer &&
                                     kl_env_on("KL_XR_FOVEA", 1);
    if (fovea_flip < 0) fovea_flip = kl_env_on("KL_XR_FOVEA_FLIP", 0);

    // KL_XR_LAYERS: composite each projection layer as its own quad with its
    // own frustum, in submission order (1, the default), or flatten them into
    // one eye picture the way this file did until 2026-08-19 (0).
    //
    // The flattening is what everything above this line is for — picking which
    // layer is the eye, deciding which of the others is a foveal inset, mapping
    // that one's frustum into the eye's and blitting it there. Every one of
    // those is a decision it can get wrong, and the seam it produced survived
    // four separate fixes to them. A per-layer composite makes none of them:
    // no layer is ever placed against another layer, only against the display.
    // 0 is kept because this is the whole picture on a target that streams, and
    // an A/B against a known state is worth more than the code it costs.
    static int layers_on = -1;
    if (layers_on < 0) {
        layers_on = kl_env_on("KL_XR_LAYERS", 1);
        if (!layers_on)
            fprintf(stderr, "  [xr] KL_XR_LAYERS=0 — projection layers are "
                            "flattened into one eye picture instead of "
                            "composited one by one\n");
    }

    // The frame's projection layers, in submission order, filed whole at the
    // end — see kl_ovrp_proj_layers_external. Built here rather than published
    // as they are found, for the reason the quad list below is: a compositor
    // reading it half-written would draw one frame's layer with another's
    // geometry.
    kl_ovrp_proj_layer pl[8];
    int npl = 0;
    // Eye 0's source size per layer, for the census only. Two layers stating
    // the SAME frustum at DIFFERENT resolutions is a shape this guest submits,
    // and the composite draws the later one — so which of them arrives last is
    // the picture's detail, and nothing else in the log carries the number.
    int plw[8] = {0}, plh[8] = {0};

    // Inset candidates are RECORDED here and applied after the layer loop, not
    // laid down as they are met. The placement needs the base's frustum and the
    // stage it was mirrored into, and a projection layer can be submitted before
    // the one we capture as the base — measured on device, where the guest
    // alternates between two and three layers and the base is the topmost, so
    // every earlier candidate was dropped with "the base layer was not captured
    // before it this frame" for whole seconds at a time. Deferring removes the
    // ordering dependency entirely rather than making it more likely to hold.
    struct { int eye, layer, w, h; uint32_t tex; XrFovf fov; float px, py; }
          insets[8];
    int   ninset = 0;
    int   base_stage_eye[2] = { -1, -1 };
    float base_tan[2][4];   // signed tan(L,R,U,D) per eye — the base frustum
    uint32_t base_tex[2] = { 0, 0 };  // the base IMAGE, for the alignment dump
    int   base_w[2] = { 0, 0 }, base_h[2] = { 0, 0 };
    float base_eye_x[2] = { 0, 0 };   // the base VIEW's position, for parallax
    float base_eye_y[2] = { 0, 0 };   // — both axes: this display's eye offset
                                      // has a real vertical component
    // What the composited layer SAYS it was drawn with. See kl_ovrp.h's
    // kl_ovrp_frame_end_external: taking these from the submission rather than
    // from our latch is what stops the compositor correcting a delta the guest
    // has already corrected.
    int   have_layer_pose = 0;
    // Zeroed, not merely assigned below: eye 1's four tangents are written only
    // when the captured layer's SECOND view is walked, and the record is
    // published on the strength of the first. A guest that submits a one-view
    // projection layer would hand the compositor a frustum of stack garbage for
    // the other eye — which presents as one eye jumping, with every counter
    // healthy. Zero is refused downstream; garbage is not.
    float layer_pose[7] = { 0 }, layer_tan[8] = { 0 };
    // The frame's non-projection layers, filed whole at the end — see
    // kl_ovrp_overlays_external. Built here rather than published as they are
    // found, because the list REPLACES the previous frame's: a compositor
    // reading it half-written would draw one frame's panel with another's.
    kl_ovrp_overlay quads[4];
    int nquads = 0;
    klxr_frame_shape(info);

    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrCompositionLayerBaseHeader *layer = info->layers[i];
        if (!layer) continue;
        if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            // Every type but the quad below is counted and dropped, and NAMED
            // once per distinct type. A count alone says content was dropped; it
            // does not say what to implement, and "every frame ignored exactly
            // one layer" is indistinguishable from "the guest composites nothing
            // we understand" — which is the state JKXR was in until the quad was
            // drawn. Cylinder and equirect are the two that would come next.
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
                static int said[8];
                static int said_n;
                int seen = 0;
                for (int k = 0; k < said_n; k++)
                    if (said[k] == (int)layer->type) { seen = 1; break; }
                if (!seen && said_n < (int)(sizeof said / sizeof said[0])) {
                    said[said_n++] = (int)layer->type;
                    fprintf(stderr, "  [xr] xrEndFrame: composition layer type %d "
                                    "is neither a projection layer nor a quad — "
                                    "counted and NOT composited (nothing here "
                                    "draws that type)\n",
                            (int)layer->type);
                }
            }
            // A quad IS the picture for a guest that submits nothing else, so
            // it is both captured and composited — the capture through the eye
            // image (which is what KL_GLFB_OUT reads), the composite through
            // the overlay record built below.
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const XrCompositionLayerQuad *q =
                    (const XrCompositionLayerQuad *)layer;
                klxr_swapchain *sc = klxr_swapchain_of(q->subImage.swapchain);
                if (sc && sc->last_released >= 0 &&
                    sc->last_released < sc->count) {
                    // eyeVisibility BOTH means one image for both eyes; LEFT
                    // and RIGHT each name one. Stated for the eye it belongs
                    // to so a two-quad guest captures the right one.
                    int eye = q->eyeVisibility == 2 ? 1 : 0;
                    kl_glfb_set_live_eye_image(eye, sc->tex[sc->last_released],
                                               (int32_t)sc->width,
                                               (int32_t)sc->height,
                                               sc->array_size > 1
                                                 ? (int)q->subImage.imageArrayIndex
                                                 : -1);
                    if (drawn_stage < 0) drawn_stage = sc->last_released;
                    if (nquads < (int)(sizeof quads / sizeof *quads) &&
                        klxr_quad_record(q, sc, &quads[nquads])) {
                        klxr_back_layer_images(sc, sc->array_size > 1
                            ? (int)q->subImage.imageArrayIndex : -1);
                        int placed = kl_glfb_layer_mtl_texture(
                                         quads[nquads].layer_id,
                                         quads[nquads].stage, NULL, NULL) != NULL;
                        klxr_quad_census(&quads[nquads], placed);
                        nquads++;
                        s->layers_quad++;
                        // Counted as composited rather than ignored: the layer
                        // reaches the compositor's list either way, and what
                        // the ignored count has always meant is content nothing
                        // downstream will ever see.
                        continue;
                    }
                }
            }
            s->layers_ignored++;
            continue;
        }
        const XrCompositionLayerProjection *proj =
            (const XrCompositionLayerProjection *)layer;
        uint32_t li = proj_layers++;
        // This layer's entry in the frame's list, opened before its views are
        // walked so a one-view layer still gets one (with the other eye left at
        // -1, which is how "this layer does not name that eye" is said).
        kl_ovrp_proj_layer *pl_cur = NULL;
        if (layers_on) {
            if (npl < (int)(sizeof pl / sizeof pl[0])) {
                pl_cur = &pl[npl++];
                memset(pl_cur, 0, sizeof *pl_cur);
                pl_cur->slot[0] = pl_cur->slot[1] = -1;
                pl_cur->pose[6] = 1.0f;          // identity orientation
            } else {
                static int said;
                if (!said++)
                    fprintf(stderr, "  [xr] more than %d projection layers in one "
                                    "frame — layer %u and any after it are NOT "
                                    "composited\n",
                            (int)(sizeof pl / sizeof pl[0]), li);
            }
        }
        for (uint32_t v = 0; v < proj->viewCount && v < 2; v++) {
            klxr_swapchain *sc = klxr_swapchain_of(proj->views[v].subImage.swapchain);
            if (!sc) continue;
            // Diagnostic (KL_GLFB_PROBE_VIDEO): read back EVERY projection
            // layer's released image, not just the one the compositor mirrors.
            // A multi-projection-layer guest (Steam Link submits three) hides
            // its picture in whichever layer we are NOT capturing, and the only
            // way to tell which is to look at all of them in one frame.
            static int probe_layers = -1;
            if (probe_layers < 0) probe_layers = kl_env_on("KL_GLFB_PROBE_VIDEO", 0);
            if (probe_layers && sc->last_released >= 0 && sc->last_released < sc->count) {
                char ptag[24];
                snprintf(ptag, sizeof ptag, "PLYR%uE%u", li, v);
                kl_glfb_probe_tex(ptag, sc->tex[sc->last_released],
                                  sc->array_size > 1
                                    ? (int)proj->views[v].subImage.imageArrayIndex
                                    : -1,
                                  (int)sc->width, (int)sc->height);
            }
            // One-time geometry census: every projection layer's reference
            // space, field of view and pose. A layer in VIEW space is
            // head-locked (a HUD/screen that follows the head); a narrow FOV is
            // a small floating panel rather than the full eye view. This is what
            // tells apart "the base layer is the immersive eye, the top ones are
            // screens" from "the top layer IS the eye" — without capturing each
            // in turn. Printed for the first few (layer,eye) pairs seen.
            {
                int slot = (int)(li * 2 + v);
                if (slot < 16 && !(g_geo_said & (1u << slot))) {
                    g_geo_said |= (uint16_t)(1u << slot);
                    klxr_space *lsp0 = klxr_space_of(layer->space);
                    const XrFovf *fv = &proj->views[v].fov;
                    const XrVector3f *lp = &proj->views[v].pose.position;
                    fprintf(stderr,
                        "  [xr] GEO layer %u eye %u: space %s  fov deg "
                        "L%.1f R%.1f U%.1f D%.1f  pose (%.2f %.2f %.2f)  "
                        "%ux%u%s\n",
                        li, v,
                        lsp0 ? klxr_ref_space_name(lsp0->reference_type) : "action/?",
                        fv->angleLeft  * 57.2958f, fv->angleRight * 57.2958f,
                        fv->angleUp    * 57.2958f, fv->angleDown  * 57.2958f,
                        lp->x, lp->y, lp->z, sc->width, sc->height,
                        (int)li == cap_layer ? " [captured]" : "");
                }
            }
            // KL_XR_FOVEA_DUMP=<dir>: EVERY projection layer's image, with the
            // frustum it states, every KL_XR_FOVEA_DUMP_EVERY seconds
            // (default 5, 0 = once).
            //
            // Layer-wide rather than the base/inset pair it started as, because
            // the pair assumed which layer was which. The first capture that
            // worked disproved the assumption outright: the WIDEST layer — the
            // one we take as the eye — was an empty blue gradient, and the
            // streamed picture was in the narrower layer we were treating as a
            // foveal inset. An instrument that only photographs the two things
            // you already believe in cannot tell you that you named them wrong.
            if (sc->last_released >= 0 && sc->last_released < sc->count &&
                (!sc->session || sc->session->gfx != KLXR_GFX_VULKAN)) {
                static const char *ddir = (const char *)-1;
                static int      every_s = -1;
                static int64_t  next_at;
                static unsigned seq;
                if (ddir == (const char *)-1) {
                    ddir = getenv("KL_XR_FOVEA_DUMP");
                    // "1" (or a relative path) means "somewhere I can actually
                    // write": on a device an absolute /tmp is outside the
                    // sandbox and every write fails with ENOENT, which is a
                    // whole run spent to learn a path. The guest's own files
                    // directory is writable by construction on both platforms.
                    if (ddir && (!*ddir || !strcmp(ddir, "1") || *ddir != '/')) {
                        static char under[640];
                        const char *base = kl_jni_files_dir();
                        snprintf(under, sizeof under, "%s/%s", base ? base : ".",
                                 (ddir && *ddir && strcmp(ddir, "1")) ? ddir : "fovea");
                        ddir = under;
                        fprintf(stderr, "  [xr] fovea dump -> %s\n", ddir);
                    }
                }
                if (every_s < 0) every_s = kl_env_int("KL_XR_FOVEA_DUMP_EVERY", 5);
                if (ddir && *ddir) {
                    int64_t now = klxr_now();
                    // One clock for the whole frame's layers, advanced by the
                    // first of them: a per-layer deadline would photograph
                    // different layers from different frames, which is exactly
                    // the comparison this exists to make impossible.
                    static int64_t frame_mark;
                    static unsigned frame_seq;
                    if (li == 0 && v == 0 && (!next_at || (every_s > 0 && now >= next_at))) {
                        next_at = every_s > 0 ? now + (int64_t)every_s * 1000000000LL
                                              : INT64_MAX;
                        frame_mark = now;
                        frame_seq = seq++;
                        if (mkdir(ddir, 0755) != 0 && errno != EEXIST) {
                            static int said;
                            if (!said++)
                                fprintf(stderr, "  [xr] fovea dump: cannot create "
                                                "%s (%s) — every write will fail\n",
                                        ddir, strerror(errno));
                        }
                    }
                    if (frame_mark) {
                        const XrFovf *df = &proj->views[v].fov;
                        char dp[512];
                        snprintf(dp, sizeof dp, "%s/eye%u_%03u_layer%u.png",
                                 ddir, v, frame_seq, li);
                        kl_glfb_dump_tex(dp, sc->tex[sc->last_released],
                                         sc->array_size > 1
                                           ? (int)proj->views[v].subImage.imageArrayIndex
                                           : -1,
                                         (int)sc->width, (int)sc->height);
                        fprintf(stderr,
                            "  [xr] dump eye %u layer %u #%03u: %ux%u tan "
                            "L%.4f R%.4f U%.4f D%.4f%s\n",
                            v, li, frame_seq, sc->width, sc->height,
                            (double)tanf(df->angleLeft), (double)tanf(df->angleRight),
                            (double)tanf(df->angleUp), (double)tanf(df->angleDown),
                            (int)li == cap_layer ? "  [captured as the eye]" : "");
                        if (li + 1 >= info->layerCount && v == 1) frame_mark = 0;
                    }
                }
            }
            // Whether this layer's picture reaches the composite. Under the
            // per-layer composite EVERY projection layer does — that is the
            // whole change — and under KL_XR_LAYERS=0 exactly one does, with
            // the rest flattened into it or dropped.
            int captured = layers_on || (int)li == cap_layer;

            // The eye textures the compositor samples, provided retroactively:
            // this call is the first moment anything knows which swapchain is
            // an eye.
            //
            // Three routes, and which one is taken is measurement rather than
            // preference. Vulkan exports the guest's own image and copies
            // nothing. GL copies the presented image (every frame), because
            // re-pointing the guest's own texture breaks some guests' rendering
            // outright — klxr_eye_mirror_on has the numbers — and because an
            // ARRAY swapchain (every Unity OpenXR guest, one texture for both
            // eyes) cannot be re-pointed at all. KL_XR_EYE_MIRROR=0 is the
            // re-point, kept as the A/B.
            //
            // `slot` is where the picture LANDED, and it is the key everything
            // downstream reads it back with — the frame record's stage and the
            // per-layer record both. Only the copy allocates one per
            // (swapchain, image); the other two routes back the guest's own
            // images in place, so their key is the image index and two
            // projection layers out of two swapchains would collide in it. No
            // guest in the corpus submits more than one projection layer on
            // those routes, and the one that tried is named rather than drawn
            // wrong.
            int slot = -1;
            if (captured) {
                if (sc->session && sc->session->gfx == KLXR_GFX_VULKAN) {
                    // One route for both shapes here: an array swapchain is a
                    // SLICE of an already-exported texture, not a copy, so the
                    // GL path's array-vs-2D split has no counterpart.
                    klxr_publish_eye_vulkan(sc, (int)v,
                                            (int)proj->views[v].subImage.imageArrayIndex);
                    slot = sc->last_released;
                } else if (klxr_eye_mirror_on() || sc->array_size > 1) {
                    slot = klxr_mirror_eye_image(sc, (int)v,
                                          (int)proj->views[v].subImage.imageArrayIndex);
                } else {
                    klxr_back_eye_images(sc, (int)v);
                    slot = sc->last_released;
                }
                if (layers_on && pl_cur && li > 0 && slot == sc->last_released) {
                    static int said;
                    if (!said++)
                        fprintf(stderr, "  [xr] projection layer %u reaches the "
                                        "compositor through its own storage, whose "
                                        "key is the image index — a second layer "
                                        "sharing an index overwrites it. Set "
                                        "KL_XR_EYE_MIRROR=1 for a slot per "
                                        "swapchain\n", li);
                }
            }

            // The image the guest DREW is the one it released, not the one its
            // framebuffer still points at — the swapchain has three and the
            // next acquire has not happened yet. Named every frame, because the
            // rotation moves every frame; the registration below is once.
            if (captured && sc->last_released >= 0 &&
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
                // The layer states its pose in ITS space, which need not be the
                // tracking space — so it is composed out of that space rather
                // than used raw. The frustum comes with it: a picture rendered
                // with one field of view must keep being placed with that one.
                klxr_space *lsp = klxr_space_of(layer->space);
                XrPosef in_tracking = klxr_pose_apply(
                    lsp ? klxr_space_pose(lsp) : (XrPosef){{0,0,0,1},{0,0,0}},
                    proj->views[v].pose);
                const XrFovf *f = &proj->views[v].fov;

                // This layer's own record, which is the per-layer composite:
                // its picture, its frustum, its pose, placed against the
                // display and never against another layer.
                if (pl_cur) {
                    if (v == 0 && npl >= 1 && npl <= 8) {
                        plw[npl - 1] = (int)sc->width;
                        plh[npl - 1] = (int)sc->height;
                    }
                    pl_cur->slot[v] = slot;
                    float *pt = pl_cur->tangents[v];
                    pt[0] = fabsf(tanf(f->angleLeft));
                    pt[1] = fabsf(tanf(f->angleRight));
                    pt[2] = fabsf(tanf(f->angleUp));
                    pt[3] = fabsf(tanf(f->angleDown));
                    // Vulkan and Metal put row 0 at the top; GL puts it at the
                    // bottom. A property of the API that drew the picture, and
                    // the same question kl_glfb_eye_mtl_origin_top_left answers
                    // for a texture it allocated.
                    pl_cur->origin_top_left =
                        sc->session && sc->session->gfx == KLXR_GFX_VULKAN;
                    // The head, as the eye pass wants it: the first view seeds
                    // the pose and the second averages its position in. The
                    // orientation is view 0's — the two are measured 0.000 deg
                    // apart, and the composite is rotation-only anyway.
                    if (v == 0) {
                        pl_cur->pose[0] = in_tracking.position.x;
                        pl_cur->pose[1] = in_tracking.position.y;
                        pl_cur->pose[2] = in_tracking.position.z;
                        pl_cur->pose[3] = in_tracking.orientation.x;
                        pl_cur->pose[4] = in_tracking.orientation.y;
                        pl_cur->pose[5] = in_tracking.orientation.z;
                        pl_cur->pose[6] = in_tracking.orientation.w;
                    } else {
                        pl_cur->pose[0] = 0.5f * (pl_cur->pose[0] + in_tracking.position.x);
                        pl_cur->pose[1] = 0.5f * (pl_cur->pose[1] + in_tracking.position.y);
                        pl_cur->pose[2] = 0.5f * (pl_cur->pose[2] + in_tracking.position.z);
                    }
                }

                // The FRAME record, which every consumer that is not the
                // per-layer pass still reads: the letterbox aspect, the
                // overlay pass's projection, the reprojection delta. With the
                // per-layer composite on, which layer fills it stops being a
                // decision about the picture — the topmost simply wins, and it
                // wins by being written last.
                //
                // The record wants the HEAD, and a projection layer states the
                // EYES — measured: eye 0's position differs from the latched
                // head by 0.0315 m, which is exactly half the IPD and not a
                // reprojection. The head is the midpoint, so the first view
                // seeds it and the second averages it in; the orientation is
                // the same for both and is taken from view 0.
                if (v == 0) {
                    if (slot >= 0) drawn_stage = slot;
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
                float *t = layer_tan + v * 4;
                t[0] = fabsf(tanf(f->angleLeft));
                t[1] = fabsf(tanf(f->angleRight));
                t[2] = fabsf(tanf(f->angleUp));
                t[3] = fabsf(tanf(f->angleDown));
                // Signed tangents + the slot the base landed in, for the foveal
                // inset below: it needs the base's real (asymmetric) frustum and
                // the eye texture the base was mirrored into. Flattening only —
                // the per-layer composite maps no layer into another's space,
                // which is the whole point of it.
                if (!layers_on) {
                    base_tan[v][0] = tanf(f->angleLeft);
                    base_tan[v][1] = tanf(f->angleRight);
                    base_tan[v][2] = tanf(f->angleUp);
                    base_tan[v][3] = tanf(f->angleDown);
                    base_eye_x[v] = proj->views[v].pose.position.x;
                    base_eye_y[v] = proj->views[v].pose.position.y;
                    base_tex[v] = sc->tex[sc->last_released];
                    base_w[v] = (int)sc->width; base_h[v] = (int)sc->height;
                    base_stage_eye[v] = slot;
                }
            }
            // Foveal inset: a NON-captured projection layer that is strictly
            // narrower than the base and centred inside it is the high-detail
            // centre of a foveated stream. Recorded here, placed after the loop
            // (see `insets`) — its placement needs the base, which may not have
            // been walked yet. GL path only; the composite is kl_glfb's blit.
            // Flattening only: with the per-layer composite on there is no base
            // to lay it over, because it is a layer like any other.
            if (fovea_on && !layers_on && (int)li != cap_layer) {
                const char *skip = NULL;
                if (!(sc->last_released >= 0 && sc->last_released < sc->count))
                    skip = "the inset swapchain has released no image";
                else if (sc->session && sc->session->gfx == KLXR_GFX_VULKAN)
                    skip = "Vulkan (this composite is kl_glfb's blit)";
                else if (ninset >= (int)(sizeof insets / sizeof insets[0]))
                    skip = "more inset candidates in one frame than can be held";
                if (skip) {
                    klxr_fovea_trace((int)v, 0, 0, 0, 0, skip);
                } else {
                    insets[ninset].eye   = (int)v;
                    insets[ninset].tex   = sc->tex[sc->last_released];
                    insets[ninset].layer = sc->array_size > 1
                        ? (int)proj->views[v].subImage.imageArrayIndex : -1;
                    insets[ninset].w     = (int)sc->width;
                    insets[ninset].h     = (int)sc->height;
                    insets[ninset].fov   = proj->views[v].fov;
                    insets[ninset].px    = proj->views[v].pose.position.x;
                    insets[ninset].py    = proj->views[v].pose.position.y;
                    ninset++;
                }
            }
            if (sc->eye == (int)v) continue;            // already this eye
            sc->eye = (int)v;
            // The capture path is GL's, and there are no GL names on the Vulkan
            // path to give it — the eyes reach a compositor through the
            // MTLTextures published above instead.
            if (!sc->session || sc->session->gfx != KLXR_GFX_VULKAN)
                for (int k = 0; k < sc->count; k++)
                    kl_glfb_note_eye_texture(sc->eye, k, sc->tex[k]);
            const XrRect2Di *r = &proj->views[v].subImage.imageRect;
            char imgs[96];
            if (sc->session && sc->session->gfx == KLXR_GFX_VULKAN)
                snprintf(imgs, sizeof imgs, "VkImages (%p %p %p)",
                         (void *)(uintptr_t)sc->vk_img[0],
                         (void *)(uintptr_t)sc->vk_img[1],
                         (void *)(uintptr_t)sc->vk_img[2]);
            else
                snprintf(imgs, sizeof imgs, "images (%u %u %u)",
                         sc->tex[0], sc->tex[1], sc->tex[2]);
            fprintf(stderr, "  [xr] layer %u eye %u <- swapchain %ux%u %s"
                            " rect %dx%d+%d+%d slice %u%s\n",
                    li, v, sc->width, sc->height, imgs,
                    r->extent.width, r->extent.height, r->offset.x, r->offset.y,
                    proj->views[v].subImage.imageArrayIndex,
                    captured ? " [captured]" : "");
        }
    }

    // The foveal insets, now that the base is known however the guest ordered
    // its layers. Everything here is the placement math that used to run inline;
    // the only change is WHEN.
    for (int k = 0; k < ninset; k++) {
        int v = insets[k].eye;
        if (base_stage_eye[v] < 0) {
            klxr_fovea_trace(v, 0, 0, 0, 0,
                             "no base layer was captured for this eye at all");
            continue;
        }
        const XrFovf *f = &insets[k].fov;
        float il = tanf(f->angleLeft),  ir = tanf(f->angleRight);
        float iu = tanf(f->angleUp),    id = tanf(f->angleDown);
        float bl = base_tan[v][0], br = base_tan[v][1];
        float bu = base_tan[v][2], bd = base_tan[v][3];
        float bw = br - bl, bh = bu - bd, ispan = ir - il;
        // CONTAINMENT, not a ratio. "Narrower than 80% of the base" was a
        // threshold with nothing behind it, and this guest's inset frustum
        // crosses it while adapting: measured on device at tan L-1.3381
        // R0.9985 (span 2.337 against the base's 2.732 — 86%, REFUSED) and,
        // two captures later, L-0.6627 R0.6737 (span 1.336 — accepted). So the
        // inset was composited on some frames and not others as the guest
        // foveated, which is the centre of the picture gaining and losing its
        // detail: the jitter, from a constant nobody measured.
        //
        // The real question is whether this layer's frustum lies INSIDE the
        // base's, which is what "an inset" means and needs no magic number. The
        // epsilon keeps a layer that merely equals the base — the same view
        // submitted twice — from being laid over itself.
        const float in_eps = 0.01f;
        int inside = il > bl + in_eps && ir < br - in_eps &&
                     iu < bu - in_eps && id > bd + in_eps;
        if (!(bw > 0 && bh > 0 && ispan > 0) || !inside) {
            klxr_fovea_trace(v, 0, 0, 0, 0,
                             "this layer's frustum is not inside the base's");
            continue;
        }
        float nx0 = (il - bl) / bw, nx1 = (ir - bl) / bw;
        float ny0, ny1;
        if (!fovea_flip) { ny0 = (bu - iu) / bh; ny1 = (bu - id) / bh; }
        else             { ny0 = (id - bd) / bh; ny1 = (iu - bd) / bh; }
        // Parallax alignment. The inset is rendered from one viewpoint and the
        // base from another, so the same angular direction points at different
        // content for anything not at infinity — seen as a doubled edge where
        // the sharp inset and the offset base overlap. A point at depth D shifts
        // by (inset - base) / D in tangent between the two viewpoints.
        // KL_XR_FOVEA_DEPTH_CM sets D; BOTH axes, because this display's
        // head->eye offset has a real vertical component where a Quest's is
        // essentially horizontal. Measured zero on Steam Link, whose two layers
        // share a pose — it costs nothing there and is not what that guest's
        // vertical seam was.
        static int depth_cm = -1, par = -1;
        if (depth_cm < 0) depth_cm = kl_env_int("KL_XR_FOVEA_DEPTH_CM", 200);
        if (par < 0)      par      = kl_env_on("KL_XR_FOVEA_PARALLAX", 1);
        float D = depth_cm / 100.0f;
        if (par && D >= 0.2f) {
            float dx = (insets[k].px - base_eye_x[v]) / D;
            float dy = (insets[k].py - base_eye_y[v]) / D;
            nx0 += dx / bw; nx1 += dx / bw;
            // Screen y runs DOWN where the tangent runs up, so the vertical
            // term enters negated — the same sign relation ny0/ny1 carries.
            ny0 -= dy / bh; ny1 -= dy / bh;
        }
        // Manual fine-alignment for the residual the depth model does not
        // reach, in THOUSANDTHS of the eye, and a scale trim about the inset's
        // own centre for doubling that grows from nothing at the centre to
        // worst at the rim (1000 = no change).
        static int shx = -32768, shy = -32768, fsc = -1;
        if (shx == -32768) shx = kl_env_int("KL_XR_FOVEA_SHIFT_X", 0);
        if (shy == -32768) shy = kl_env_int("KL_XR_FOVEA_SHIFT_Y", 0);
        if (fsc < 0)       fsc = kl_env_int("KL_XR_FOVEA_SCALE", 1000);
        nx0 += shx / 1000.0f; nx1 += shx / 1000.0f;
        ny0 += shy / 1000.0f; ny1 += shy / 1000.0f;
        if (fsc > 0 && fsc != 1000) {
            float sc_ = fsc / 1000.0f;
            float cx = (nx0 + nx1) * 0.5f, cy = (ny0 + ny1) * 0.5f;
            nx0 = cx + (nx0 - cx) * sc_; nx1 = cx + (nx1 - cx) * sc_;
            ny0 = cy + (ny0 - cy) * sc_; ny1 = cy + (ny1 - cy) * sc_;
        }
        // Once per frame shape: the numbers behind a vertical seam. `as placed`
        // is after every correction above, so it is what the blit did; the flip
        // figure is the delta a flip would add to it, and it is a pure shift
        // whose size is ((iu+id) - (bu+bd)) / bh — zero only for a frustum
        // symmetric about the view axis, which this one is not.
        // Printed on a frame-shape change AND whenever the inset's own frustum
        // moves materially, because this guest's foveation is DYNAMIC — the
        // inset narrows and widens while the shape stays the same, and those are
        // exactly the seconds klxr_fovea_trace reports placement travel in. A
        // shape-keyed line cannot describe an event it never fires on.
        static float said_iu[2], said_id[2];
        int moved = fabsf(iu - said_iu[v]) > 0.002f ||
                    fabsf(id - said_id[v]) > 0.002f;
        if (!(g_fovea_said & (1u << v)) || moved) {
            g_fovea_said |= (uint8_t)(1u << v);
            said_iu[v] = iu; said_id[v] = id;
            fprintf(stderr,
                "  [xr] fovea eye %d placement: base tan U%.3f D%.3f at "
                "(%.4f %.4f), inset tan U%.3f D%.3f at (%.4f %.4f); y "
                "[%.4f..%.4f] as placed; KL_XR_FOVEA_FLIP would move it %.1f "
                "thousandths of the eye\n",
                v, (double)bu, (double)bd,
                (double)base_eye_x[v], (double)base_eye_y[v],
                (double)iu, (double)id, (double)insets[k].px,
                (double)insets[k].py, (double)ny0, (double)ny1,
                (double)((((iu + id) - (bu + bd)) / bh) * 1000.0f));
        }
        klxr_fovea_trace(v, nx0, ny0, nx1, ny1, NULL);
        kl_glfb_overlay_eye_inset(v, base_stage_eye[v], insets[k].tex,
                                  insets[k].layer, insets[k].w, insets[k].h,
                                  nx0, ny0, nx1, ny1);
    }

    // The Vulkan frame seam, and BONELAB's trap (c) reached through the other
    // API: a compositor's "is there a new frame?" test is a SERIAL, and on the
    // Vulkan path that serial is kl_vulkan's. Nothing else advances it — the GL
    // fence below is 0 forever here, because there is no ANGLE queue — so
    // without this call every eye texture above is correctly published, the
    // counters are all healthy, and the display never updates. That failure has
    // no error surface at all, which is why it gets a line of its own rather
    // than riding inside the present block.
    //
    // It also makes the guest's queue idle first, which is what turns "the
    // frame was submitted" into "the picture is complete" — the eye MTLTextures
    // are the guest's own images, so nothing of ours is in its submissions and
    // there is no MTLSharedEvent to wait on instead.
    if (drawn_stage >= 0 && s->gfx == KLXR_GFX_VULKAN)
        kl_vulkan_frame_done(drawn_stage);

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
    // ---------------------------------------------------------------------
    // Publishing this frame, and the ORDER of it is load-bearing.
    //
    // A compositor waits on the guest's frame fence and then draws whatever
    // records it finds. If a record is published BEFORE the fence value that
    // covers it, the compositor can read the fence for frame N, read the record
    // for frame N+1, and draw N+1's layers having waited only for N's blits —
    // so a destination still holds its previous contents and one frame of the
    // display is a picture from several frames ago. That is the "stale frame"
    // flash, and it is a pure ordering bug: every call succeeds and the picture
    // is correct again on the next frame.
    //
    // So the frame's completion is published FIRST (the Vulkan serial, or the
    // GL fence signal inside kl_glfb_present) and the records that describe it
    // SECOND. A compositor that reads the records before the fence then cannot
    // hold a record newer than the fence it is about to wait on — it can only
    // hold an older one, which costs a frame of latency and is always safe.
    // See KleptonCompositor.encodeFrame and klvm_draw_proj_layers, which read
    // in that order for this reason.

    // Close this frame's record, under the image the guest actually presented.
    // A frame that submitted no projection layer for the composited layer drew
    // no new picture, so nothing is filed and the compositor shows the previous
    // one again — the same rule, and for the same reason, as klovrp_EndFrame's
    // "a frame that drew into no eye stage must not file anything".
    if (drawn_stage >= 0)
        kl_ovrp_frame_end_external(drawn_stage,
                                   have_layer_pose ? layer_pose : NULL,
                                   have_layer_pose ? layer_tan : NULL);

    // ...and this frame's PROJECTION layers, in the order the guest submitted
    // them, replaced whole for the same reason the panels below are. Filed only
    // when the per-layer composite is on: a count of zero is what tells every
    // compositor to fall back to the single eye picture, which is what every
    // other guest and KL_XR_LAYERS=0 must keep getting.
    if (layers_on) {
        kl_ovrp_proj_layers_external(pl, npl);
        s->proj_hist[npl < 9 ? npl : 8]++;
        for (int k = 0; k < npl && k < 8; k++) {
            s->proj_present[k]++;
            if (pl[k].slot[0] >= 0 || pl[k].slot[1] >= 0) s->proj_placed[k]++;
        }
        // The frame's layer ARRANGEMENT, printed whenever it changes — which is
        // often, and is the point. This guest alternates between two and three
        // layers about once a second AND narrows one layer's frustum while it
        // runs, so a count-keyed line describes a shape the run spends most of
        // its time not being in. Keyed on the fields of view rather than on the
        // slots, which rotate every frame by design.
        char shape[192];
        int  sn = snprintf(shape, sizeof shape, "%d:", npl);
        for (int k = 0; k < npl && sn > 0 && sn < (int)sizeof shape; k++)
            sn += snprintf(shape + sn, sizeof shape - (size_t)sn, " %.0fx%.0f@%dx%d",
                           (double)((atanf(pl[k].tangents[0][0]) +
                                     atanf(pl[k].tangents[0][1])) * 57.2958f),
                           (double)((atanf(pl[k].tangents[0][2]) +
                                     atanf(pl[k].tangents[0][3])) * 57.2958f),
                           plw[k], plh[k]);
        static char said_shape[192];
        if (strcmp(shape, said_shape) != 0) {
            snprintf(said_shape, sizeof said_shape, "%s", shape);
            fprintf(stderr, "  [xr] compositing %d projection layer(s) back to "
                            "front:", npl);
            for (int k = 0; k < npl; k++)
                fprintf(stderr, " [%d] slots %d/%d fov %.0fx%.0f deg, %dx%d px",
                        k, pl[k].slot[0], pl[k].slot[1],
                        (double)((atanf(pl[k].tangents[0][0]) +
                                  atanf(pl[k].tangents[0][1])) * 57.2958f),
                        (double)((atanf(pl[k].tangents[0][2]) +
                                  atanf(pl[k].tangents[0][3])) * 57.2958f),
                        plw[k], plh[k]);
            fprintf(stderr, "\n");
        }
    }

    // ...and this frame's panels, replaced whole — including with none, which is
    // how a guest taking a menu down is expressed. Unconditional rather than
    // guarded on nquads: leaving the previous frame's list standing would keep
    // drawing a layer the guest has stopped submitting.
    kl_ovrp_overlays_external(quads, nquads);
    // The same statement for the eye, which had no way to be told "not this
    // frame" — see kl_ovrp.h. A guest that alternates between a projection pair
    // and a quad shows one or the other, not both at once.
    kl_ovrp_eye_layer_external(proj_layers > 0);

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
// other — klxr_space_pose_ex and klxr_pose_rel, and nothing else here. No pair
// is special-cased: VIEW located in a static space is the case where the left
// operand carries a rotation, anything located in VIEW falls out of the same
// composition, and an ACTION space is a pose in the tracking space like the
// others. The only thing distinguishing an action space is that it can be
// UNTRACKED, which is a flags answer rather than a different computation.
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
    int sp_motion_known = 0;
    XrPosef sp_pose = klxr_space_pose_ex(sp, &sp_tracked, lin, ang, &sp_motion_known);
    XrPosef bs_pose = klxr_space_pose_ex(bs, &bs_tracked, NULL, NULL, NULL);

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
    // alternative is an unfilled struct becoming the client's basis for pose
    // prediction.
    int base_static = bs->reference_type == KLXR_REF_SPACE_LOCAL ||
                      bs->reference_type == KLXR_REF_SPACE_STAGE;
    // ...and only if it is a measurement. Without this the derived-velocity
    // seam would report zeros as valid for exactly the samples that have no
    // basis, which is the lie this whole path exists to avoid.
    int have_motion = sp->reference_type == 0 && base_static && sp_motion_known;
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
        {"xrGetVulkanGraphicsRequirementsKHR",
                                   (void *)klxr_GetVulkanGraphicsRequirementsKHR},
        {"xrGetVulkanInstanceExtensionsKHR",
                                   (void *)klxr_GetVulkanInstanceExtensionsKHR},
        {"xrGetVulkanDeviceExtensionsKHR",
                                   (void *)klxr_GetVulkanDeviceExtensionsKHR},
        {"xrGetVulkanGraphicsDeviceKHR",
                                   (void *)klxr_GetVulkanGraphicsDeviceKHR},
        {"xrEnumerateDisplayRefreshRatesFB",
                                   (void *)klxr_EnumerateDisplayRefreshRatesFB},
        {"xrGetDisplayRefreshRateFB", (void *)klxr_GetDisplayRefreshRateFB},
        {"xrRequestDisplayRefreshRateFB",
                                   (void *)klxr_RequestDisplayRefreshRateFB},
        {"xrConvertTimespecTimeToTimeKHR",
                                   (void *)klxr_ConvertTimespecTimeToTimeKHR},
        {"xrConvertTimeToTimespecTimeKHR",
                                   (void *)klxr_ConvertTimeToTimespecTimeKHR},
        {"xrPerfSettingsSetPerformanceLevelEXT",
                                   (void *)klxr_PerfSettingsSetPerformanceLevelEXT},
        {"xrSetAndroidApplicationThreadKHR",
                                   (void *)klxr_SetAndroidApplicationThreadKHR},
        {"xrEnumerateColorSpacesFB", (void *)klxr_EnumerateColorSpacesFB},
        {"xrSetColorSpaceFB",        (void *)klxr_SetColorSpaceFB},
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
        {"xrGetActionStateVector2f",(void *)klxr_GetActionStateVector2f},
        {"xrGetActionStatePose",   (void *)klxr_GetActionStatePose},
        {"xrEnumerateBoundSourcesForAction",
                                   (void *)klxr_EnumerateBoundSourcesForAction},
        {"xrGetInputSourceLocalizedName",
                                   (void *)klxr_GetInputSourceLocalizedName},
        {"xrApplyHapticFeedback",  (void *)klxr_ApplyHapticFeedback},
        {"xrStopHapticFeedback",   (void *)klxr_StopHapticFeedback},
        // swapchains — the eye images, and the compositor seam
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

// --- the space algebra, with no session and no guest ------------------------
//
// `make xrspace`. The bug this catches is invisible from anywhere else: every
// call returns success and the picture is correct, and the only other
// instrument is a human turning their head inside a live stream — which costs a
// fresh Steam pairing and cannot be repeated identically.
//
// The invariant klxr_space_pose exists to keep: an answer given RELATIVE to the
// head must not contain the head's own position. Model
// VIEW as a y displacement of zero and the eye-to-head comes back as the eye's
// absolute position in the tracking space, so SteamVR rotates the eye about a
// point as far from it as the head is from the origin and the view swings
// through an arc instead of turning in place.
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

// --- the action surface, with no session and no guest -----------------------
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
    g_active_profile[0] = 0;
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

        // ...and the correction actually REACHES the grip pose. A pitch that
        // misses its pose has no other symptom — the position is right, the
        // space is tracked, every call returns XR_SUCCESS — and applied to aim
        // spaces alone it moves nothing a user can see, because the controller
        // they are looking at IS the grip pose.
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

    // Haptics: what is checked is that the order REACHES kl_ovrp's queue — the
    // frontend half of that seam is M8's, gated by `make haptics`.
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

    // ---- the Steam Frame profile (VRChat) ----------------------------------
    //
    // This is the section that made the active profile dynamic. VRChat is the
    // guest that did: it suggests ONLY /interaction_profiles/valve/\n    
    // frame_controller_valve (48 bindings), NEVER Touch, so with Touch
    // hardcoded as active every one of its suggestions decoded "inactive
    // profile" and the guest's controllers were dead. The map below is that
    // guest's own, transcribed from a real run and trimmed to the controls
    // that matter; the assertions are the ones that have to hold for VRChat to
    // have input at all, and none of them could be made against the Touch-only
    // section above because a Valve-bound action never decodes there.
    klxr_st_boot();   // fresh state: this guest has no other profile
    {
        XrActionSetCreateInfo vsi; memset(&vsi, 0, sizeof vsi);
        vsi.type = XR_TYPE_ACTION_SET_CREATE_INFO;
        snprintf(vsi.actionSetName, sizeof vsi.actionSetName, "%s",
                 "steamframecontroller");
        void *vset = NULL;
        klxr_CreateActionSet(&g_instance, &vsi, &vset);

        void *vstick = klxr_st_action(vset, "thumbstick", KLXR_ACTION_TYPE_VECTOR2F);
        void *vtop   = klxr_st_action(vset, "facebuttontop", KLXR_ACTION_TYPE_BOOLEAN);
        void *vbot   = klxr_st_action(vset, "facebuttonbottom", KLXR_ACTION_TYPE_BOOLEAN);
        void *vgrip  = klxr_st_action(vset, "grippressed", KLXR_ACTION_TYPE_BOOLEAN);
        void *vmenu  = klxr_st_action(vset, "menu", KLXR_ACTION_TYPE_BOOLEAN);
        void *vtrig  = klxr_st_action(vset, "triggerpressed", KLXR_ACTION_TYPE_FLOAT);
        void *vpose  = klxr_st_action(vset, "devicepose", KLXR_ACTION_TYPE_POSE);

        const XrActionSuggestedBinding valve_map[] = {
            klxr_st_sb(vstick, "/user/hand/left/input/thumbstick"),
            klxr_st_sb(vstick, "/user/hand/right/input/thumbstick"),
            klxr_st_sb(vtrig,  "/user/hand/left/input/trigger/value"),
            klxr_st_sb(vtrig,  "/user/hand/right/input/trigger/value"),
            klxr_st_sb(vmenu,  "/user/hand/right/input/menu/click"),
            klxr_st_sb(vmenu,  "/user/hand/left/input/view/click"),
            klxr_st_sb(vgrip,  "/user/hand/left/input/squeeze/click"),
            klxr_st_sb(vgrip,  "/user/hand/right/input/squeeze/click"),
            klxr_st_sb(vtop,   "/user/hand/right/input/y/click"),
            klxr_st_sb(vtop,   "/user/hand/left/input/dpad_up/click"),
            klxr_st_sb(vbot,   "/user/hand/right/input/a/click"),
            klxr_st_sb(vbot,   "/user/hand/left/input/dpad_down/click"),
            klxr_st_sb(vpose,  "/user/hand/left/input/grip/pose"),
            klxr_st_sb(vpose,  "/user/hand/right/input/grip/pose"),
        };
        klxr_st_bind_all(KLXR_VALVE_PROFILE,
                         valve_map, sizeof valve_map / sizeof valve_map[0]);

        // The whole reason the active profile is not a constant: a guest that
        // suggests ONLY the Valve frame still gets a live map, right down to
        // the face-button collapse the user agreed to (right y aliases onto
        // the right controller's B, left dpad_up onto its Y).
        ok &= klxr_st_ok(f, "Valve: bindings are taken for a profile-only guest",
                         ((klxr_action *)vtop)->kind[1] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vtop)->bit[1] == KL_OVRP_RAW_B &&
                         ((klxr_action *)vtop)->kind[0] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vtop)->bit[0] == KL_OVRP_RAW_Y);
        ok &= klxr_st_ok(f, "Valve: facebuttonbottom is right A / left dpad_down",
                         ((klxr_action *)vbot)->bit[1] == KL_OVRP_RAW_A &&
                         ((klxr_action *)vbot)->bit[0] == KL_OVRP_RAW_X);
        ok &= klxr_st_ok(f, "Valve: view/click is the BACK bit, menu the START bit",
                         ((klxr_action *)vmenu)->kind[0] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vmenu)->bit[0] == KL_OVRP_RAW_BACK &&
                         ((klxr_action *)vmenu)->kind[1] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vmenu)->bit[1] == KL_OVRP_RAW_START);
        ok &= klxr_st_ok(f, "Valve: squeeze/click is the hand-trigger threshold",
                         ((klxr_action *)vgrip)->kind[0] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vgrip)->bit[0] == KL_OVRP_RAW_LHAND_TRIGGER &&
                         ((klxr_action *)vgrip)->kind[1] == KLXR_SRC_BUTTON &&
                         ((klxr_action *)vgrip)->bit[1] == KL_OVRP_RAW_RHAND_TRIGGER);
        // The whole-stick path decodes to a kind only the Vector2f getter
        // reads — the guest's locomotion input, unreachable without both.
        ok &= klxr_st_ok(f, "Valve: /input/thumbstick is a Vector2f source",
                         ((klxr_action *)vstick)->kind[0] == KLXR_SRC_STICK_VEC &&
                         ((klxr_action *)vstick)->kind[1] == KLXR_SRC_STICK_VEC);

        // And the profile is REPORTED as the Valve frame, not hardcoded Touch
        // — the two are not the same controller and the guest keys off this.
        XrPath left = 0;
        klxr_StringToPath(&g_instance, "/user/hand/left", &left);
        XrInteractionProfileState ips;
        memset(&ips, 0, sizeof ips);
        klxr_GetCurrentInteractionProfile(&g_session, left, &ips);
        ok &= klxr_st_ok(f, "Valve: the profile reported is the Valve frame",
                         ips.interactionProfile != 0 &&
                         strcmp(klxr_path_str(ips.interactionProfile),
                                KLXR_VALVE_PROFILE) == 0);

        // End to end: a stick pushed on the right hand must read back through
        // xrGetActionStateVector2f — the entry point that did not exist. The
        // vector is captured at sync from the same snapshot every other action
        // reads, so this is not two methods of one seam disagreeing.
        kl_ovrp_set_hand_pose(1, 0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
        kl_ovrp_set_controller_input(1, 0, 0, 0, 0, -0.5f, 0.25f);
        kl_ovrp_frame_latch();
        klxr_st_sync();
        XrActionStateVector2f vv;
        memset(&vv, 0, sizeof vv);
        XrActionStateGetInfo vgi;
        memset(&vgi, 0, sizeof vgi);
        vgi.type = XR_TYPE_ACTION_STATE_GET_INFO;
        vgi.action = vstick;
        klxr_StringToPath(&g_instance, "/user/hand/right", &vgi.subactionPath);
        XrResult vrr = klxr_GetActionStateVector2f(&g_session, &vgi, &vv);
        ok &= klxr_st_ok(f, "Valve: the stick reads through xrGetActionStateVector2f",
                         vrr == KLXR_SUCCESS && vv.isActive &&
                         fabsf(vv.currentState.x + 0.5f) < 1e-4f &&
                         fabsf(vv.currentState.y - 0.25f) < 1e-4f);
        // The combine rule is the FLOAT rule (larger magnitude), and it must
        // hold for vectors too: movement is the hand pushing hardest. Publish
        // the LEFT hand pushing harder and read both — the answer must be the
        // left vector, not a sum (-1.3) and not first-hand-wins.
        kl_ovrp_set_hand_pose(0, -0.2f, 1.0f, -0.3f, 0, 0, 0, 1);
        kl_ovrp_set_controller_input(0, 0, 0, 0, 0, -0.8f, 0.1f);
        kl_ovrp_frame_latch();
        klxr_st_sync();
        memset(&vv, 0, sizeof vv);
        vgi.subactionPath = 0;
        klxr_GetActionStateVector2f(&g_session, &vgi, &vv);
        ok &= klxr_st_ok(f, "Valve: combined read takes the larger push, not a sum",
                         vv.isActive && fabsf(vv.currentState.x + 0.8f) < 1e-4f &&
                         fabsf(vv.currentState.y - 0.1f) < 1e-4f);

        // Bound-source enumeration: the stick action is bound on both hands,
        // and the two-call shape (0 capacity -> count) is the only shape a
        // guest is allowed to rely on.
        XrBoundSourcesForActionEnumerateInfo ei;
        memset(&ei, 0, sizeof ei);
        ei.type = XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO;
        ei.action = vstick;
        uint32_t cnt = 0;
        ok &= klxr_st_ok(f, "Valve: bound sources enumerate (count first)",
                         klxr_EnumerateBoundSourcesForAction(&g_session, &ei, 0,
                                                             &cnt, NULL) ==
                             KLXR_SUCCESS && cnt == 2);
        XrPath srcs[2] = {0, 0};
        ok &= klxr_st_ok(f, "Valve: ...and fill on the second call",
                         cnt == 2 &&
                         klxr_EnumerateBoundSourcesForAction(&g_session, &ei, 2,
                                                             &cnt, srcs) ==
                             KLXR_SUCCESS && srcs[0] != 0 && srcs[1] != 0);

        // Localized names: requested only ever for display, but a source path
        // must never refuse one — Unity's input catalogue calls this to build
        // its bindings UI, and VRChat's was the first guest that asked.
        char nb[64];
        uint32_t nneed = 0;
        XrInputSourceLocalizedNameGetInfo lni;
        memset(&lni, 0, sizeof lni);
        lni.type = XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO;
        klxr_StringToPath(&g_instance, "/user/hand/right/input/thumbstick",
                          &lni.sourcePath);
        XrResult lr = klxr_GetInputSourceLocalizedName(&g_session, &lni,
                                                       sizeof nb, &nneed, nb);
        ok &= klxr_st_ok(f, "Valve: a source path has a display name",
                         lr == KLXR_SUCCESS &&
                         strstr(nb, "Right Controller") != NULL);
    }

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

        // poseInReferenceSpace. In VIEW it must be ROTATED by the head, not
        // merely added to it: a metre in
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
