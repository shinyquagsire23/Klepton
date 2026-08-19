# Klepton

Running Android ARM64 VR APKs on Apple Vision Pro, no JIT required!

## Architecture

`klepton-ld` translates Android `.so` libraries into loadable Apple `.dylib` and `.framework` libraries, which then link into the Klepton runtime. Klepton currently focuses on Java-thin applications only (no ART, no JVM).

For graphics, GLES 3.2 is translated to a vendored ANGLE GLES 3.0 (with its Metal backend), and Vulkan is translated to MoltenVK.

```
┌─────────────────────────────────────────────────────────────────┐
│  GUEST (translated Mach-O, instruction bytes mostly unmodified) │
│  libil2cpp · libunity · libunityopus · libmain · burst · ...    │
└─────────────────────────────────────────────────────────────────┘
    ↕ imports resolved to Klepton runtime in `klepton-ld`
┌─────────────────────────────────────────────────────────────────┐
│  libklepton_bionic   libc/libm/libdl/pthread/liblog → libSystem │
│  libklepton_ndk      ALooper · ANativeWindow · ASensor · AAsset │ 
│  libklepton_jni      synthetic JavaVM / JNIEnv                  │
│  libklepton_ovrp     ovrp_* reimplementation                    │
│  ...                                                            │
└─────────────────────────────────────────────────────────────────┘
    ↕ Frontend platform (`mains`) and OS-specifics
┌─────────────────────────────────────────────────────────────────┐
│  MoltenVK (Vulkan → Metal)     ANGLE (GLES 3.0)                 │
│  Compositor Services · ARKit · GameController · AVAudioEngine   │
└─────────────────────────────────────────────────────────────────┘
```

While both Android and macOS reserve x18, some (a lot of) older Android applications still use x18, and macOS zeros x18 on context switches. All usage of x18 is patched by `klepton-ld` so that per-library TLS slots are used instead.

Klepton also has the ability to load and patch `.so` files at runtime with `mmap`, but this is only really useful on macOS where JIT is actually allowed. It's likely that some applications will require JIT if they happen to utilize scripting runtimes that expect it (LuaJIT, V8, whatever else).

## Building

See [`BUILDING.md`](BUILDING.md) for the full guide. The short version:

```bash
brew install pkg-config sdl3 apktool          # host dependencies
apktool d -f -o beatsaber beatsaber.apk       # you supply the APK; see BUILDING.md
make check                                    # full regression sweep
```

Quick run scripts:

```bash
./build_run_viewer.sh [target] # build and run macOS frontend
./build_run_vpro.sh [target]  # build and run Vision Pro frontend
./build_run_host.sh [target]  # build and run a guest headless, with a summary
./build_run_host.sh steamlink-vr --shell --view  # Steam VR Link frontend (WIP)
```

Where `[target]` is the name of the target, defaulting to `beatsaber`.

## Status

Currently working well on both macOS and visionOS with minor-to-no graphical issues:

- Beat Saber versions 1.6, 1.28, and 1.40
- BONELAB
- SUPERHOT VR
- Resident Evil 4 VR
- OpenBrush (OpenXR)
- OpenJK XR (Team Beef)

WIP or not in a great state:

- Steam VR Link (no AWDL mitigations due to entitlement restrictions, XR_EXT_hand_tracking not implemented yet)

Abandoned due to anticheat, JIT mmaping, or other problems:

- VRChat (Steam Frame APK)