// Surreal Interactive's own controller API, as linked from
// Frameworks/surreal_interactive_openxr_framework.xcframework
// (github.com/surreal-interactive/SwiftSDKBinary, Apache-2.0).
//
// The framework ships OpenXR headers only. These declarations are the C API
// the same archive exports (controller_capi.o), with the signatures of
// SDK/Assets/SVR/Scripts/Core/SVRInputApi.cs in github.com/surreal-interactive/SDK
// (Apache-2.0): C# int is int32_t, long is int64_t. Nothing here is called
// unless the app is built with KL_SURREAL_SDK, which gen_xcodeproj.py sets only
// when the framework is present, so a checkout without it still builds.
#ifndef KL_SURREAL_SDK_H
#define KL_SURREAL_SDK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { float x, y, z; float qw, qx, qy, qz; } SVRPose;
typedef struct { float x, y, z; } SVRVector3f;
typedef struct {
    uint8_t primary_button, secondary_button, menu_button, stick_z_button;
    uint8_t trigger_button_value, grip_button_value, stick_x_value, stick_y_value;
} SVRButtons;
typedef void (*SVRButtonCallback)(int64_t timestamp, int32_t chirality, SVRButtons buttons);

void SVRStart(void);
void SVRStop(void);
// chirality 0 = left, 1 = right. hand_skeletons may be NULL (Unity passes IntPtr.Zero).
bool SVRPollDevicePose(int32_t chirality, int64_t poll_timestamp, SVRPose *pose,
                       SVRVector3f *linear_velocity, SVRVector3f *angular_velocity,
                       void *hand_skeletons);
void SVRSetButtonCallback(SVRButtonCallback callback);
// amplitude [0, 1], frequency (20, 300].
bool SVRHapticContinuous(int32_t chirality, float amplitude, float frequency, double duration_seconds);
bool SVRIsConnected(int32_t chirality);
int64_t SVRTimeNow(void);
// The poll a rendered frame used, and when that frame finished: Unity reports
// each poll three frames later, and the SDK predicts its poses from this.
void SVRRenderFinish(int32_t chirality, int64_t poll_timestamp, int64_t render_finish_timestamp);

#endif
