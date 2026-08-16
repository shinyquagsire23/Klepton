# Building Klepton

macOS on Apple Silicon only. Everything here assumes an arm64 host — the project
runs guest ARM64 code natively, so there is no x86_64 path and never will be.

The build is a plain `Makefile` plus a few shell scripts. There is no CMake, no
package manager for the project itself, and no git submodules.

---

## TL;DR

```bash
# host deps
brew install pkg-config sdl3 apktool

# put the APKs you supply in the repo root, then unpack them
apktool d -f -o <target> <target>.apk

make check                               # regression sweep, builds everything in the process
make angle-all                           # pull, patch and build ANGLE (slow)
make mvk                                 # MoltenVK, for the Vulkan targets

./build_run_viewer.sh <target>           # build and run the macOS viewer for a target
./build_run_vpro.sh <target>             # build and run the visionOS app bundle for a target
```

`make check` needs no ANGLE, no NDK and no headset. Everything past it does.

---

## Host requirements

Known-good versions are what this tree is developed against, not minimums —
where a version is load-bearing it says so.

| dependency | known-good | needed for | how to get it |
|---|---|---|---|
| macOS on Apple Silicon | 26.5.1 (Darwin 25.5) | everything | — |
| Xcode + Command Line Tools | 26.0.1 (17A400), Apple clang 17.0.0 | everything | App Store; `xcode-select --install` |
| macOS SDK | 26.0 | host builds | ships with Xcode |
| xrOS + xrSimulator SDKs | 26.0 | `make xros`, visionOS app | Xcode → Settings → Components → visionOS |
| Python 3 | 3.14.6 (3.8+ is plenty) | `tools/*.py`, `visionos/gen_xcodeproj.py` | ships with Xcode CLT, or `brew install python` |
| `pkg-config` | any | locating SDL3 | `brew install pkg-config` |
| SDL3 | 3.4.12 | macOS interactive viewer (**optional**) | `brew install sdl3` |
| apktool | 2.11.1 | unpacking the guest APKs | `brew install apktool` |
| ANGLE (vendored, patched) | ANGLE `25e7211`, depot_tools `6afa997` | any real-GL path | `make angle-all` |
| MoltenVK (vendored, prebuilt) | `v1.4.2` | any Vulkan path (BONELAB, Open Brush, RE4) | `make mvk` |
| Android NDK | r25c (`25.2.9519653`) | `make guest` only (**optional**) | Android Studio SDK Manager |
| Vision Pro + Apple Developer account | — | device runs only | — |

visionOS signing is auto-detected from your keychain's `Apple Development`
certificate; override with `KLEPTON_TEAM=<teamid>`. The Simulator needs neither.

The bundle id is set to `$USER.dev.klepton.target.<target>` in order to avoid collisions.
Set `KLEPTON_BUNDLE_SCOPE` to use something else instead of `$USER`, or `KLEPTON_BUNDLE_ID` 
to override the the whole id.

---

## Guest applications

**Do not ask me where to get these.** You supply them.

| target | APK | unpacked to | graphics | also needs |
|---|---|---|---|---|
| Beat Saber 1.40.8 | `beatsaber.apk` | `beatsaber/` | GLES (ANGLE) | one OBB, 1.3 GB |
| SUPERHOT VR 1.161 | `superhot.apk` | `superhot/` | GLES (ANGLE) | — |
| BONELAB 1.2974 | `bonelab.apk` | `bonelab/` | Vulkan (MoltenVK) | two OBBs, 6.3 GB |
| VRChat 2026.2.3 | `vrchat.apk` | `vrchat/` | GLES (ANGLE) | — |
| Open Brush 2.30.0 | `openbrush.apk` | `openbrush/` | Vulkan (MoltenVK) | — |
| RE4 VR 2.3 | `re4.apk` | `re4/` | Vulkan (MoltenVK) | two OBBs, 7.9 GB |
| Steam Link 2.0.20 (VR) | `steamlink-vr.apk` | `steamlink-vr/` | GLES (ANGLE) | — |
| Steam Link (flat) | `steamlink-android.apk` | `steamlink-android/` | — | `make x18-slink` only |

Unpack with **apktool**:

```bash
apktool d -f -o beatsaber    beatsaber.apk
apktool d -f -o superhot     superhot.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
# ... etc
```

**Keep the `.apk` files themselves.** `getPackageCodePath()` hands Unity the APK path
and Unity opens it as a zip.

Additional targets can be defined in `visionos/targets.py`; run `make targets`
afterwards to regenerate the runtime's copy of the table
(`runtime/kl_target_table.h`, which is committed).

To build a target:

```bash
make check TARGET=<target>          # the host gates
./build/m_boot <target>             # host run
./build_run_vpro.sh <target>        # on the headset
```

Steam Link is the only target with separate mains of its own, still working on unifying that.

### OBBs (split application binaries)

Some targets ship their data beside the APK rather than inside it. The runtime
reads the OBB out of the guest's userdata directory:

```
~/Library/Application Support/Klepton/userdata/<target>/<obb path>/
```

| target | OBB path under userdata | files |
|---|---|---|
| `beatsaber` | `obb/` | `main.1716.com.beatgames.beatsaber.obb` |
| `bonelab` | `obb/` | `main.2974` + `patch.2974`, `.com.StressLevelZero.BONELAB.obb` |
| `re4` | `Android/obb/com.Armature.VR4/` | `main.203` + `patch.203`, `.com.Armature.VR4.obb` |

---

## ANGLE

```bash
make angle-all       # pull + patch + all three slices, from nothing
```

ANGLE requires a lot of disk space to build, budget ~20 GiB total for both the
source checkout and build intermediates:

| | |
|---|---|
| sources (`third_party`, `buildtools`, `depot_tools`, `.git`) | ~12 GiB |
| `out/Debug` — host slice | 4.4 GiB |
| `out/ios` + `out/ios-sim` | ~1.4 GiB |
| `out/xros` + `out/xrsim` — the retargeted frameworks | 24 MiB |
| **`vendor/` total** | **~18 GiB** |


`vtool` is used to retarget the iOS builds to visionOS.

---

## MoltenVK

```bash
make mvk             # pull the pinned prebuilt + stage all three slices
```

---

## Build Targets

Everything below builds what it needs first. `<target>` is any name from
`python3 visionos/targets.py --list` and defaults to Beat Saber.

| want | command |
|---|---|
| a guest on macOS, in a window | `./build_run_viewer.sh <target>` |
| a guest on macOS, headless | `./build/m_boot <target>` |
| a guest on the visionOS Simulator | `KLEPTON_TARGET=<target> visionos/run.sh` |
| a guest on Vision Pro | `./build_run_vpro.sh <target>` |
| Steam Link config UI, macOS | `./build_run_slink.sh --shell --view` |
| Steam Link shim work list, in seconds | `./build_run_slink.sh --gap` |
| visionOS build gate, no device needed | `make xros` |

---

## Working on ANGLE itself

Skip unless you are editing `vendor/`.

```bash
make angle-save      # export vendor/'s delta -> angle-patches/klepton.patch
make angle-status    # pin, HEAD, local delta, whether the patch is current
make angle-sync      # force the DEPS re-resolve that angle-fetch skips
```
