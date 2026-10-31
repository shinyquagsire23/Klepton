# Surreal Touch controller backend

This is an independent, integrated BLE backend for the **myMartek/Klepton `avp-play` branch**. Both Standard and Surreal sources compile into every generated game. Host, build and synthetic simulator checks pass. Both physical controllers connected on Vision Pro, and the tester confirmed working buttons, good hand-derived position and substantially improved sensor-only rotation. **Native positional tracking, fast-motion accuracy, radio recovery and physical vibration remain unverified.**

## Source and dependency provenance

Inspected on 2026-10-14:

| Repository | Revision |
| --- | --- |
| myMartek/avp-play | `5b47a0acd4dd708c3a17f4aa29a5813148b41af9` |
| myMartek/Klepton, avp-play | `be35c2542b93766543218a876ab08e55bd45ef8b` (published source snapshot of toolchain 507) |
| globo128/OpenSurreal | `f4dd94de16d1f628d901a6eea25cdaa4648b70fa` (reference only) |
| shinyquagsire23/SurrealTouch_re | `0bba3170c189c318b373c130560d76090304ab60` (reference only; control message types, inspected 2026-10-23) |
| surreal-interactive/SwiftSDKBinary | `a42deb007c7f12afb543b235b75464e86e00125a` (Apache-2.0; linked into Surreal builds, see below) |
| surreal-interactive/SDK | Unity SDK, `Assets/SVR/Scripts/Core/SVRInputApi.cs` (Apache-2.0; the C API signatures, inspected 2026-10-28) |

OpenSurreal has no LICENSE/COPYING file or express redistribution grant in this revision. Its source was inspected to establish its public APIs and interoperability facts, including UUIDs, packet layouts, handedness naming and coordinate conventions. **None of its implementation, UI, spatial session or package is copied, vendored, linked or downloaded by this build.** This is not a claim that the reference project is permissively licensed or that this was a clean-room process. Obtain an explicit license before replacing this backend with OpenSurreal or distributing any of that project's code. SurrealTouch_re likewise has no license file; only the control message type numbers and TLV header layout were taken from its notes, and none of its code is used.

New implementation files follow this repository's MIT license. The one third-party runtime dependency is Surreal Interactive's own SDK (Apache-2.0, `third-party-licenses/SurrealInteractiveSDK-LICENSE.txt`), in Surreal builds only. Apple CoreBluetooth, ARKit, QuartzCore and SwiftUI are SDK frameworks. Existing ANGLE/MoltenVK licensing remains in `third-party-licenses`; the validation build uses their prebuilt slices from AVP Play release v1.1.15, toolchain `507-1954ecf`, verified against the release sidecar SHA-256.

## Selection and precedence

- Mac: `AVPPlayCore.ControllerMode`, persisted under `controllerMode`; default `standard`.
- Build: `KLEPTON_CONTROLLER_MODE=standard|surreal` becomes `KL_CONTROLLER_MODE_DEFAULT` and the built Info.plist's `KLControllerModeDefault`.
- Headset: `klepton.controllerMode` in this game's UserDefaults overrides the bundle default. An invalid value falls back to the bundle default, then Standard.
- `KleptonControllers` freezes the mode when the compositor is created. Exit and reopen the app to change modes after starting a game. Changing the Mac preference does not pair controllers or rewrite installed headset preferences.
- Standard follows the original Sense/hand/gamepad code. Surreal bypasses Sense polling, gamepad polling, accessory-provider discovery and pinch events. A missing controller is released and parked out of view; the other remains usable.
- Both routes use `submit(_:hand:)` and the existing `kl_ovrp_set_hand_motion` / `kl_ovrp_set_controller_input` seam. Game haptics are pulled from the same existing runtime queue.

## Pose source: Surreal Interactive's SDK

In Surreal mode a game takes its controller poses, buttons and haptics from Surreal Interactive's own visionOS SDK (`KleptonSurrealSDK.swift`), not from the Bluetooth backend described below. Every build links the SDK, whichever mode it starts in (`KLEPTON_CONTROLLER_MODE` is only the start window's first choice), so a game installed for standard controllers can be switched to Surreal Touch in its start window and uses the SDK too. In Standard mode nothing calls it.

Why: the controllers track themselves, and their tracking frame drifts away from the headset's. With only the backend below, which can register a controller only from a still grip hold, the hands ended 15–45 cm off near the head and 20–30 cm off in fast swings on a Vision Pro (Half-Life: Alyx, 2026-10-28): ammo from the backpack failed more than half the time and throws went sideways. The SDK solves that alignment continuously — clock sync with the controllers, the latency between them and hand tracking, the controller-to-world and hand-to-controller transforms, whether a controller is held — and on the same hardware the hands stayed with the player's own.

- **Chosen at run time.** `KleptonControllers` reads the mode when the game starts and uses the SDK in Surreal mode wherever the build has it (`KleptonSurrealSDK.enabled`). `gen_xcodeproj.py` fetches it the first time a build does not have it (`tools/surreal_sdk_fetch.sh`, also `make surreal-sdk` to stage it ahead of time): Surreal's `SwiftSDKBinary` at a pinned commit, verified by SHA-256, into `visionos/Frameworks/` (gitignored). A build that starts in Surreal mode and cannot have it stops with the reason rather than quietly shipping the Bluetooth backend; any other build goes on without it and says so, so a failed download never stops a game that has nothing to do with Surreal Touch. `KLEPTON_SURREAL_SDK=0` builds without it, on purpose.
- **How it is linked.** A static framework: linked, never embedded, with `-framework Accelerate` for its solver, and `KL_SURREAL_SDK` as the Swift condition that lets `KleptonSurrealSDK.swift` call it. Its OpenXR half is not used and not linked (nothing references it); `kl_surreal_sdk.h` declares the small C API the same archive exports, with the signatures of Surreal's Unity SDK.
- **What it owns.** Its own CoreBluetooth central (it connects the controllers paired in Settings → Bluetooth) and its own ARKit hand-tracking session. `KleptonControllers` therefore disconnects `KleptonSurrealBackend` before starting it, and the launcher does not connect the backend at all in such a build: its Surreal panel says the controllers are connected and aligned by Surreal's software when the game starts, and keeps only the two button settings (analog full scale, invert stick Y), which the SDK's buttons go through as well. Hand tracking in `KleptonCompositor` keeps running alongside it.
- **Per frame.** As Surreal's Unity integration does: `SVRPollDevicePose` at `SVRTimeNow()`, and each poll reported with `SVRRenderFinish` three frames later, which is how the SDK learns the render latency it predicts across. Buttons arrive through `SVRSetButtonCallback` and go through the same 13-byte decoding as the backend, so the Touch mapping, analog scale, dead zone and stick touch are identical.
- **Three facts found on hardware.** The C API is the one Unity consumes, so its poses are left-handed with +Z forward; ARKit is right-handed with −Z forward, and z is mirrored (position and linear velocity negate z, orientation and angular velocity negate x and y). Until it has aligned a controller it answers position (100, 100, 100), which is treated as untracked. And `SVRStart` creates a new provider, so `SVRSetButtonCallback` must come after it — set before, no button ever arrives.
- **Cursor mode, before the SDK starts.** The SDK connected both controllers but no button ever arrived, and they kept driving the visionOS pointer (red light): it does not take them out of cursor mode on its own (Vision Pro, 2026-10-29). Sent once it holds them, the backend's control messages could not help either: a second connection from the same app stayed in "connecting" for good. So `SurrealModeKicker` runs first: it connects the Surreal Touch controllers visionOS already has, writes the mode request and set-controller-mode messages (`0x0D`, `0x0C 0`, see *Wire input* below) on `1004`, waits for each controller's reply on `1006` (at most 3 s; at once if none is connected), closes its link, and only then calls `SVRStart`. Kept open, that link stopped the SDK from connecting; closed, the controllers stay in controller mode (green light). On that headset the replies went from cursor to controller mode, the SDK connected 1.2 s later and buttons arrived.
- **Hands or controllers, never both.** Fingers from hand tracking around a held controller shook and twisted it, so the two are alternatives. Five grip squeezes on both controllers within three seconds (`SurrealResetGesture`, read from the SDK's buttons) switch to **hands**: the hand path Standard mode uses (the hand's pose, pinch as trigger), the controllers ignored but still listened to for the gesture; the same gesture switches back to **controllers**, and because they came back misaligned after hands mode, the SDK is restarted (`SVRStop`, the cursor-mode step, `SVRStart`, about 2.4 s with the controllers parked) so that it aligns them where they are. Each switch releases all input and gives both controllers a short pulse. The game sees the squeezes as grips too. The render thread stops calling the SDK while it restarts.
- **Smoothed hands.** In hands mode the predicted ARKit wrist shook the hands. A One Euro filter (`KleptonPoseSmoother`: cutoff 1 Hz plus 8 Hz per m/s, rotation 1 Hz plus 1.5 Hz per rad/s) smooths the wrist once per hand and frame, and both the hand's pose and its OVR skeleton hang from it, so the fingers stay on the hand. A still hand's tracker noise is mostly removed; a fast motion passes with little lag. `KL_HAND_SMOOTH=0` turns it off, `KL_HAND_SMOOTH_MIN` and `KL_HAND_SMOOTH_BETA` tune it. Only in Surreal's hands mode.
- **The backend.** `KleptonSurrealBackend` stays for a build without the SDK: one made with `KLEPTON_SURREAL_SDK=0`, or a Standard build whose download failed.
- **Switched off.** `KLEPTON_SURREAL_SUPPORT=0` (AVP Play's *Surreal Touch support* off) builds a game for standard controllers only: Standard mode whatever was asked, no SDK fetched or linked, and `KLSurrealAvailable` NO in its Info.plist, so the start window shows no controller type and no Surreal panel, and a Surreal choice stored by an earlier build is put back to Standard (`KleptonControllerMode.available`).

Connection events go to `Documents/surreal-sdk.log`, written off the render thread.

## Pairing and calibration

This section and the ones after it describe the Bluetooth backend (`KleptonSurrealBackend`), which a Surreal build uses only when built without the SDK. In the game launcher, open Settings → Controllers, select Surreal Touch, turn on the controllers and select Discover. Connect the explicitly identified left and right devices. Names without an unambiguous Surreal Touch L/R token are rejected rather than guessed. Pairing, connection, battery (optional SIG battery service), input values, recalibration and vibration controls live on Vision Pro.

The last successful peripheral UUID for each hand is remembered locally. Reconnect retrieves those UUIDs without scanning. A lost link clears that hand immediately and retries on the BLE queue. Disconnect cancels retry for that hand. Bluetooth permission/power failures clear input and report status. There is no pairing on the Mac.

Subscribing alone does not start the input stream. Until it receives a control message, a controller keeps driving the visionOS pointer over HID (cursor mode) and sends nothing on pose/input: on a Vision Pro on 2026-10-23, both controllers subscribed without error and only the battery level arrived. Once both input subscriptions are confirmed, the backend therefore writes a mode request (TLV `0x0D`) and then sets controller mode (`0x0C`, argument `0`) on the control characteristic `1004`, and traces the replies from `1006` (`0x0E`, argument `0` = controller mode). Message types follow shinyquagsire23/SurrealTouch_re. In that test the first pose packet arrived 2 ms after the first control message, about 100 pose packets per second followed per controller, and both replies reported controller mode.

Discover includes peripherals already connected by visionOS, because system-connected controllers may no longer advertise. A system Bluetooth connection alone is not enough: select the controller in this app so it can subscribe to pose and button notifications. Disconnect and Vibration test appear only for a ready controller; an in-progress connection has a separate Cancel connection action. Connection events and first packet lengths are written to the local `Documents/surreal-ble.log` (capped at 256 KiB per launch) for hardware diagnostics. Pose/button streams are not logged per frame, and this logging never runs on the render thread.

After Start, hold each controller still with the corresponding hand visible and squeeze its grip. This explicitly registers the controller's native frame into the world. A reconnect, timestamp reset or stale/invalid pose requires calibration again. Initial calibration is intentionally not persisted across ARKit world origins or controller power cycles.

To recalibrate without leaving the game, squeeze the grip five times on both controllers within three seconds. A squeeze is a press above 0.55 after a release below 0.35, so a held grip counts once and light touches do not count. Both controllers vibrate and the registration of both is discarded, as with **Reset forward alignment**; calibrate again as after Start. The game also receives those squeezes, so do it with empty hands. The gesture is traced to `surreal-ble.log`.

`KleptonCompositor` remains the sole ARKit-session owner. Its existing predicted hand anchors are converted by the existing `gripPose` convention and supplied to the Surreal cache. No second HandTrackingProvider or WorldTrackingProvider is started. World registration is gently corrected while the grip is held and native speed is below 0.35 m/s and 1.5 rad/s. Fast motion and occluded hands use the controller's native pose relative to the last registration. Releasing the grip freezes registration, so an empty hand cannot drag a controller sitting nearby.

### Optional position from hand tracking

In Settings → Controllers → Surreal Touch, enable **Position from hand tracking** before Start. It is **off by default**, persisted per game as `klepton.surreal.positionFromHands`, and never enabled by choosing Surreal Touch on the Mac. With it off, native positioning above is unchanged.

With it on, each controller's position comes directly from the matching compositor hand's predicted grip point. Rotation and angular velocity still come from the Surreal pose; buttons, triggers and sticks come only from the physical controller. After Start, look straight ahead and point both controllers level and forward, with sticks facing up  Keep hands visible, hold each grip still for half a second, then release. This explicitly defines forward from the level headset heading; it never uses wrist rotation. Rotation calibration is averaged over at least 350 ms of a stable reference (within 5 degrees, native angular speed below 0.35 rad/s and grip above 0.55), then latched. Grip release, hand loss, fast rotation or a reference discontinuity restarts an incomplete calibration. Once calibrated, holding or pressing grip during play cannot reorient the controller from hand-tracking estimates. Use **Reset forward alignment** to explicitly restart alignment; disconnect, stale BLE or changing position mode also reset it. Manual direction adjustments do not reset registration.

Calibration uses rotation only in this mode. Native position, native linear velocity and native positional confidence do not gate hand positioning; a fresh pose packet with a valid quaternion is still required. Linear velocity is computed from consecutive visible hand positions at presentation time, without a second prediction step. The native-position mode retains its existing continuous registration behavior.

A missing hand freezes its last position for at most 150 ms with zero linear velocity. Longer occlusion parks that controller and releases its game inputs until that hand returns; the other hand is independent. Reacquisition starts with zero linear velocity and the latest physical button snapshot, avoiding a false throw or resurrected release. Stale BLE data or disconnect still releases all inputs even with a visible hand. Explicit forward reset or changing position mode clears both registration and hand-position history. Hand occlusion alone retains orientation registration.

This option depends on the cameras seeing the hand while it holds a controller. Occlusion, latency and alignment during real swings must be evaluated on hardware; synthetic tests do not establish gameplay quality.

### Optional direction adjustment

The default needs no multi-pose calibration. With hand positioning enabled, **Adjust direction (+ / −)** opens a small panel and two live lines, with no controller models or pose-fitting wizard. Point the controllers level and forward with sticks up, look ahead, and choose **Center forward**. Adjust Up/down, Left/right and Twist by one degree with + / −, independently per hand; **Reset to 0°** removes the offsets. The short tick on each line makes twist visible. Changes persist immediately for this game, with a zero default and limits of ±90°. **Done** returns to the launcher.

Every ray's orientation comes from BLE; only its position comes from the matching tracked hand. Center forward fixes a sensor-to-world rotation. Offsets are applied afterward (`world × device × userOffset`, local X/Y/Z order), so recentering or the initial game alignment does not cancel the user's adjustments. Old experimental guided-fit profiles are not used. Position-mode changes, reconnects and stale BLE invalidate registration; hand occlusion alone does not. Lost tracking hides the preview ray. This is an optional comfort adjustment, not a measurement of firmware tracking accuracy.

The preview owns an ARKit session only before gameplay and stops it on exit or startup failure. Starting the game is disabled and guarded while it is open. Gameplay retains the compositor's existing ARKit session. Native-position mode retains its original manual pitch setting; the optional aim offsets are also applied after its world registration.

## Wire input and runtime mapping

The interoperability reference describes a custom service `6E401001-B5A3-F393-E0A9-E50E24DCCA9E`, pose/input/haptic characteristics with suffixes `1002`/`1003`/`1004`, and optional standard battery service `180F` / characteristic `2A19`.

| Input | Mapping |
| --- | --- |
| Left primary / secondary | Oculus raw X `0x100` / Y `0x200` |
| Right primary / secondary | raw A `0x1` / B `0x2` |
| Left / right stick click | raw `0x400` / `0x4` |
| Left menu / right capture | raw Start `0x100000` / Back `0x200000` |
| Trigger / grip | analog index / hand trigger; raw digital threshold 0.55 |
| Sticks | center 128, signed normalization, radial 0.12 dead zone, unit-circle clamp, optional invert Y |
| Capacitive touch | unavailable in the described input packet; only touches the input proves are set — pressed face buttons and stick, and a stick pushed past the dead zone (Half-Life: Alyx's teleport is a SteamVR dpad with `sub_mode: touch`) |

Buttons use a 13-byte packet (uint64 time, flags, trigger, grip, stick X/Y). Pose packets contain uint64 nanosecond time and 17 float32 values: position, xyzw quaternion, confidence, linear/angular velocity, acceleration. Invalid lengths, nonfinite numbers and degenerate quaternions are rejected. Z is reflected on position/linear vectors; X/Y are negated on quaternion imaginary components and angular velocity, preserving the axial-vector convention. Orientations are normalized.

The reference explicitly labels grip full scale and stick polarity as assumptions. Diagnostics allow 127 or 255 analog full scale and stick Y inversion. Those settings need checking against the actual firmware. No extra system/home button or touch sensor is invented.

Haptics encode the device's 13-byte command, with normalized amplitude mapped to 0–10000, Hz clamped to 20–300 (runtime pulses use 170 Hz) and durations constrained to 30–250 ms. Requests are coalesced per hand. Writes honor GATT properties/backpressure, and response writes do not overlap. Hardware buffered envelopes are approximated by short pulses; sub-30-ms waveforms and frequency fidelity require device testing. The existing runtime pull API supplies amplitude/duration but no frequency.

## Synchronization and prediction

CoreBluetooth owns a dedicated serial queue. The renderer reads a short lock-protected value cache; it never waits for BLE operations or dispatches synchronously to that queue. Packet parsing happens before taking the renderer's cache lock. Haptics use a replaceable pending slot rather than accumulating delayed work.

A constant-memory lower-envelope estimator maps device nanosecond timestamps onto `CACurrentMediaTime`, with clock reset detection and a small clock-drift allowance. One-way BLE timing cannot determine absolute transport delay. Pose prediction uses world-aligned linear/angular velocities and the frame presentation timestamp, capped at 35 ms; it never extrapolates backward. Registration-frame translation/rotation rates are included in published velocities while correcting. Samples older than 250 ms release buttons and analogs and discard registration. Input snapshots are retained between change notifications, including across a stale pose stream or clock reset, because a button held through the gap sends nothing new; only disconnect clears them. Pose gaps, clock resets, pose-confidence changes and grip press/release are traced to `surreal-ble.log` from the BLE queue.

## Build and validation

From the runtime checkout:

```sh
make surreal                  # decoder/mapping/fusion tests + real project generator checks
make check TARGET=beatsaber140 # existing runtime gates; needs your cached/unpacked game
make -j4 xros                 # real device and simulator runtime compile/link gates
visionos/SurrealHarness/build.sh
```

The diagnostic app compiles the same controller files as the game. With a booted visionOS 26+ simulator:

```sh
xcrun simctl install booted visionos/build/SurrealHarness.app
xcrun simctl launch booted io.avpplay.surreal-diagnostics --surreal-demo --self-test
```

The orange SIMULATED label is mandatory. `--surreal-demo` is compiled only for Simulator and cannot enable fake input on physical Vision Pro. The integration suite covers streaming, handedness, analog input, disconnect/reconnect, simulated haptic routing, mode persistence, hand positioning, sensor-only rotation and manual aim adjustment without losing registration. Results are saved in the harness's Documents/surreal-self-test.json. `--aim-preview` opens the line-only UI with synthetic BLE rotation and illustrative positions. It does not claim real hand tracking or physical calibration. The harness contains no game content.

For a game build, follow BUILDING.md using the same purchased APK and bundle ID as its existing installation. Example (once dependencies and your game tree are present):

```sh
KLEPTON_TARGET=beatsaber140 visionos/mkguest.sh
visionos/mkangle.sh
KLEPTON_TARGET=beatsaber140 KLEPTON_CONTROLLER_MODE=surreal KLEPTON_START_SCREEN=1 python3 visionos/gen_xcodeproj.py
xcodebuild -jobs 2 -project visionos/Klepton.xcodeproj -scheme Klepton \
  -sdk xros -destination 'generic/platform=visionOS' ARCHS=arm64 \
  CODE_SIGNING_ALLOWED=NO build
```

The last command validates an unsigned product; it does not make an installable signed game. AVP Play's normal installer supplies the existing team and bundle ID and signs it. No controller-specific entitlement was added. The existing Bluetooth, hands and world privacy strings remain in both the plist and generated build settings.

## Hardware acceptance still required

1. Back up saves with AVP Play's existing save feature. Keep the existing game version, bundle prefix, Apple team and per-game toolchain selection.
2. Select the modified toolchain explicitly; choose Surreal Touch in Mac settings. Use Rebuild Controllers for an already installed game with cached data. This runs only the existing build/install step, never fetch, stage, uninstall or save restoration.
3. Pair L/R in the headset launcher, confirm battery/input values and the physical vibration test. Start, hold still and squeeze grip with hands visible.
4. Verify correct hands, world position, grip orientation at multiple yaw/pitch/roll angles, trigger, grip, sticks and all face/menu buttons in Beat Saber. Check fast swings, throws, occlusion, drift and reacquisition.
5. Power off one controller while holding input. Confirm the other remains active and no button is stuck. Reconnect, recalibrate, repeat for the other hand and for Bluetooth off/on.
6. Quit; choose Standard; relaunch without rebuilding. Verify Sense/hand/gamepad behavior separately with available hardware.

The simulator cannot establish radio compatibility, battery reads from hardware, physical tracking quality, real haptic strength, or correct Saber/Touch grip offsets. OpenSurreal's sample can be run separately as a personal reference if you have permission to do so; it was not built or redistributed here.
