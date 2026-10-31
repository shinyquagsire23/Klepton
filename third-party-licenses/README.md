# Licences of the bundled binaries

Besides the source of this repository (MIT, see `LICENSE`), a toolchain package contains
prebuilt third-party libraries:

| Part | Where in the package | Licence | Text |
|---|---|---|---|
| ANGLE (libEGL, libGLESv2), with the changes in `angle-patches/klepton.patch` | `vendor/out/xros`, `vendor/out/xrsim` | BSD-3-Clause | `ANGLE-LICENSE.txt` |
| MoltenVK | `vendor-moltenvk/out` | Apache-2.0 | `MoltenVK-LICENSE.txt` |
| Surreal Interactive's visionOS SDK (`surreal_interactive_openxr_framework`, github.com/surreal-interactive/SwiftSDKBinary at a42deb0), linked into Surreal Touch builds only | `visionos/Frameworks/surreal_interactive_openxr_framework.xcframework` (fetched by `tools/surreal_sdk_fetch.sh`) | Apache-2.0 | `SurrealInteractiveSDK-LICENSE.txt` |

It also carries third-party source that is compiled with the runtime:

| Part | Where in the package | Licence | Text |
|---|---|---|---|
| Fossilize (Valve), at 556da9f, library sources only | `vendor-fossilize` | MIT | `vendor-fossilize/LICENSE` |
| RapidJSON (part of Fossilize) | `vendor-fossilize/rapidjson` | MIT | `vendor-fossilize/rapidjson/license.txt` |
| miniz (part of Fossilize) | `vendor-fossilize/miniz` | MIT | `vendor-fossilize/miniz/LICENSE` |
| Vulkan headers (Khronos, part of Fossilize) | `vendor-fossilize/khronos` | Apache-2.0 or MIT | head of each file |
| SMOL-V, at 55000ef, one marked change | `vendor-smolv` | MIT or public domain | `vendor-smolv/LICENSE.txt` |

Recording and pre-building the Vulkan pipelines (`runtime/gfx/kl_fossilize.cpp`,
`runtime/gfx/kl_vk_pipe_hooks.h`, `tools/alyx_pipelines.py`) follows Glovebox
(github.com/glovebox-vr/glovebox, MIT); each file says so at its head.

The game app's Steam connection (`visionos/SteamCloudKit`, sign-in and Steam Cloud saves) consists of
parts of Glovebox's `SteamLauncherKit` (MIT, `visionos/SteamCloudKit/LICENSE-Glovebox.txt`); what was taken
and what was changed is in `visionos/SteamCloudKit/README.md`.

The rewrite of Half-Life: Alyx's eye shaders that ends the colour flashes in NPC eyes (`runtime/gfx/kl_vk_eyefix.c`)
is Glovebox's `klvk_eye_envguard` (MIT, same licence file), widened from one shader module to the six that have the
fault; the file says what was changed.

Four smaller things follow Glovebox as well (MIT, same licence file), each named where it is: the game's files
kept out of the device backup (`visionos/Sources/KleptonBackup.swift`), the GPU ids put into Half-Life: Alyx's
video settings file at every start (`kl_vulkan_gpu_ids` in `runtime/gfx/kl_vulkan.c`, `alyx_video_gpu_ids` in
`visionos/Sources/kl_app.c`), the previous run's log kept as `klepton-boot.prev.log` (`kl_app.c`), and a left
dominant hand driving the bindings from the other controller (`runtime/xr/kl_openxr.c`).

The patch against the ANGLE source is complete in `angle-patches/klepton.patch`.
Game content and libraries translated from it are never part of a package.
