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

```bash
./build_run_viewer.sh # build and run macOS Beat Saber frontend
./build_run_vpro.sh   # build and run Vision Pro Beat Saber frontend
./build_run_slink.sh  # macOS Steam VR Link frontend (WIP)

make check            # full regression sweep
```

## Status

Beat Saber is working on macOS and visionOS with minor graphical issues, Steam VR Link and improving generalizability/build tooling is still WIP.
