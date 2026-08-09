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
| Android NDK | r25c (`25.2.9519653`) | `make guest` only (**optional**) | Android Studio SDK Manager |
| Vision Pro + Apple Developer account | — | device runs only | — |

visionOS signing is auto-detected from your keychain's `Apple Development`
certificate; override with `KLEPTON_TEAM=<teamid>`, bundle id with
`KLEPTON_BUNDLE_ID` (default `dev.klepton.app`). The Simulator needs neither.

---

## Guest applications

**Do not ask me where to get these.** You supply them.

| target | APK | unpacked to | needed by |
|---|---|---|---|
| Beat Saber (Quest 1.28.0) | `beatsaber.apk` | `beatsaber/` | `make check`, everything Beat Saber |
| Steam Link (VR build) | `steamlink-vr.apk` | `steamlink-vr/` | `make slink-vr`, `slink-main`, `slink-shell` |
| Steam Link (flat build) | `steamlink-android.apk` | `steamlink-android/` | `make x18-slink` only |

Unpack with **apktool**:

```bash
apktool d -f -o beatsaber    beatsaber.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
```

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
