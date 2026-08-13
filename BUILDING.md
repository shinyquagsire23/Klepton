# Building Klepton

macOS on Apple Silicon only. Everything here assumes an arm64 host — the project
runs guest ARM64 code natively, so there is no x86_64 path and never will be.

The build is a plain `Makefile` plus a few shell scripts. There is no CMake, no
package manager for the project itself, and no git submodules.

---

## TL;DR

```bash
brew install pkg-config sdl3 apktool     # host deps
# put beatsaber.apk / steamlink-vr.apk in the repo root, then unpack them
# (see "Guest applications" — they are not distributable and not in this repo)
apktool d -f -o beatsaber   beatsaber.apk
apktool d -f -o steamlink-vr steamlink-vr.apk

make check                               # the regression sweep — start here
make check TARGET=superhot               # ...against another guest, same gates
make angle-all                           # pull, patch and build ANGLE (slow)
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
| ANGLE (vendored, **patched**) | ANGLE `25e7211`, depot_tools `6afa997` | any real-GL path | `make angle-all` |
| MoltenVK (vendored, **prebuilt**) | `v1.4.2` | any Vulkan path (BONELAB) | `make mvk` |
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

| target | APK | unpacked to | needed by |
|---|---|---|---|
| Beat Saber (Quest 1.28.0) | `beatsaber.apk` | `beatsaber/` | `make check`, everything Beat Saber |
| SUPERHOT VR (Quest) | `superhot.apk` | `superhot/` | `make check TARGET=superhot`, `./build/m_boot superhot` |
| Steam Link (VR build) | `steamlink-vr.apk` | `steamlink-vr/` | `make slink-vr`, `slink-main`, `slink-shell` |
| Steam Link (flat build) | `steamlink-android.apk` | `steamlink-android/` | `make x18-slink` only |

Unpack with **apktool**:

```bash
apktool d -f -o beatsaber    beatsaber.apk
apktool d -f -o superhot     superhot.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
```

Additional targets can be defined in `visionos/targets.py` for the project generation.


**Keep the `.apk` files themselves.** `getPackageCodePath()` hands Unity the APK path
and Unity opens it as a zip.

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
make mvk-check       # ...and prove it: link for xros, then RUN it on the host
make mvk-status      # what platform and floor each staged slice carries
```

The host side of a synthetic `libvulkan.so` — BONELAB boots completely and
cannot render because its graphics API is Vulkan (`notes/BONELAB.md`). Seconds
and ~360 MB, not ANGLE's ~18 GiB and hour: it is vendored as a **prebuilt
release tarball**, because unlike ANGLE nothing here patches it. If that ever
changes, this becomes a source checkout and the asymmetry goes away.

`vendor-moltenvk/` is gitignored like `vendor/`, and kept separate from it on
purpose — `vendor/` *is* the ANGLE checkout, with ANGLE at its root, so anything
else in there would be an untracked stowaway inside another project's git. The
tracked artifacts are `tools/mvk_fetch.sh`, `tools/mvk_retarget.sh`, and the
version + sha256 pin inside the former.

Two things invert what the ANGLE section above says, and both are the reason
this is cheap:

- **The platform needs no forgery.** Khronos ships native `xros-arm64` and
  `xros-arm64_x86_64-simulator` slices in `MoltenVK-all.tar`, already stamped
  `VISIONOS` / `VISIONOSSIMULATOR`. ANGLE's iOS→visionOS `vtool` trick is not
  needed here and would be strictly worse than a real build. **Use
  `MoltenVK-all.tar`, not `MoltenVK-ios.tar`** — the latter carries an
  `ios-arm64` device slice and no visionOS anything.
- **The deployment floor does.** The release is built against the visionOS
  26.5 SDK and stamps `minos 26.5`; dyld refuses an image whose minimum exceeds
  the OS running it, and this tree's SDK is 26.0. `tools/mvk_retarget.sh` lowers
  it to 1.0 with `vtool` and rewrites `MinimumOSVersion` in the framework's
  `Info.plist` — the plist carries the floor a second time, and left alone it is
  a bundle-validation failure at *install* time, before dyld ever runs.

Lowering the floor is safe rather than hopeful: the same tarball's iOS slice is
built from these sources with `minos 15.0`, so nothing in MoltenVK needs a 26.x
API. `make mvk-check` is what settles it instead of arguing it — it links a
probe against the retargeted device slice with the local xrOS SDK
(`-Wl,-no_weak_imports`, so a symbol that would be missing at runtime on the
lowered floor is a link error), and then loads the macOS slice and brings up a
real `VkInstance`, enumerating a physical device by name. Known-good on an
M1 Max: `Apple M1 Max — Vulkan 1.0.357`.

| slice | platform | used by |
|---|---|---|
| `vendor-moltenvk/out/macos/libMoltenVK.dylib` | MACOS, floor 12.0, untouched | the host loop |
| `vendor-moltenvk/out/xrsim/MoltenVK.framework` | VISIONOSSIMULATOR, floor 1.0 | the Simulator |
| `vendor-moltenvk/out/xros/MoltenVK.framework` | VISIONOS, floor 1.0 | the device |

The simulator slice ships fat (`x86_64` + `arm64`) and is thinned to `arm64`
before `vtool`, which rewrites one architecture at a time.

---

## The Actual Build Targets

Everything below builds what it needs first.

| target | command |
|---|---|
| **regression sweep** — start here | `make check` |
| Beat Saber, macOS, in a window | `./build_run_viewer.sh` |
| Beat Saber, visionOS Simulator | `visionos/run.sh` |
| Beat Saber, Vision Pro | `visionos/run.sh device` |
| Steam Link config UI, macOS | `./build_run_slink.sh --shell --view` |
| Steam Link shim work list, in seconds | `./build_run_slink.sh --gap` |
| visionOS build gate, no device needed | `make xros` |

`visionos/run.sh` runs the whole device loop — runtime, guest translations,
ANGLE wrapping, project, install, asset staging, launch. `KL_SKIP_STAGE=1`
skips the 2.2 GB asset upload once it is staged, which on device is the
difference between a 20-second loop and a 20-minute one.

Occasional: `make x18` / `x18-slink` (decoder vs objdump), `make guest` (needs
the NDK), `make dylibs` / `bootdylib` (the guest from translated dylibs),
`make mtltex` / `reproject` / `trace` / `haptics`, `make clean`.

---

## Working on ANGLE itself

Skip unless you are editing `vendor/`.

```bash
make angle-save      # export vendor/'s delta -> angle-patches/klepton.patch
make angle-status    # pin, HEAD, local delta, whether the patch is current
make angle-sync      # force the DEPS re-resolve that angle-fetch skips
```

`vendor/` is gitignored, so **an ANGLE edit that has not been through
`make angle-save` exists on exactly one machine.** The tracked artifact is
`angle-patches/klepton.patch`, re-applied to a fresh checkout; commit
granularity lives in `vendor/`'s own `klepton` branch.

`make angle-fetch` is safe to re-run on a tree you have modified — it never
writes to an existing `vendor/.git`, and the solution is `"managed": False` so
`gclient sync` cannot reset the checkout out from under you. The full reasoning,
and the pins, are in the ANGLE block of the `Makefile`.
