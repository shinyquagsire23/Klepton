CC      := clang
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64
LDLIBS  := -lz -framework AudioToolbox
# The host/ship split is a source-list boundary, not a runtime getenv (§12.2).
#
# RUNTIME_SHIP is everything that goes into the visionOS app bundle. It is the
# list `make xros` builds against the xrOS SDK, so anything added here has to
# compile for the device — which is the point of keeping it separate rather
# than discovering the divergence at port time.
#
# RUNTIME_DIAG is host-only instrumentation: the sampling profiler and the
# managed-side probe. Nothing in RUNTIME_SHIP includes their headers (only
# tests/m_boot.c does), so the boundary holds by construction rather than by
# discipline. runtime/kl_view.c (SDL viewer) is host-only too and is named by
# the m_boot rule alone.
RUNTIME_SHIP := runtime/kl_env.c runtime/kl_image.c runtime/kl_stub_cells.S runtime/kl_shim.c runtime/kl_va.c \
           runtime/kl_va_handlers.c runtime/kl_va_thunks.S \
           runtime/kl_libc.c runtime/kl_libc_slink.c runtime/kl_pthread.c runtime/kl_dl.c \
           runtime/kl_ndk.c runtime/kl_jni.c runtime/kl_x18.c \
           runtime/kl_egl.c runtime/kl_opensl.c runtime/kl_audio.c runtime/kl_ovrp.c \
           runtime/kl_ovrp_sret.S runtime/kl_reproject.c runtime/kl_present.c \
           runtime/kl_ovrplat.c runtime/kl_mediandk.c runtime/kl_glfb.c runtime/kl_gl_trace.S runtime/kl_gl_lock.S \
           runtime/kl_il2cpp.c runtime/kl_fault.c
RUNTIME_DIAG := runtime/kl_sample.c runtime/kl_mprobe.c
RUNTIME := $(RUNTIME_SHIP) $(RUNTIME_DIAG)

.PHONY: all test clean check load vatest il2cpp boot jnislots x18 guest
all: build/t_opus

build/t_opus: tests/t_opus.c $(RUNTIME) runtime/klepton.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_opus.c $(RUNTIME) $(LDLIBS)

test: build/t_opus
	./build/t_opus beatsaber/lib/arm64-v8a/libunityopus.so

clean:
	rm -rf build

build/t_load: tests/t_load.c $(RUNTIME) runtime/klepton.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_load.c $(RUNTIME) $(LDLIBS)

LIBS := beatsaber/lib/arm64-v8a
load: build/t_load
	@for f in libmain lib_burst_generated libunityopus libunity libil2cpp; do \
	  ./build/t_load $(LIBS)/$$f.so || true; echo; done

# SL-1 — the second target's boot harness (PLANNING §11). Links the same
# runtime as m_boot, minus the host-only diagnostics: this target has no
# managed side to probe and no viewer yet.
#
# It links the SDL viewer for the same reason m_boot does: Steam Link is a FLAT
# app, so the viewer is not a debugging aid here — it is the app's actual
# output device. kl_view_mtl.m and tests/t_mtl_provider.m come with it because
# kl_view.c references both; the mono path uses neither (no eye textures to
# provide, and the readback sink needs no Metal interop), but the symbols must
# resolve.
build/m_slink: mains/m_slink.c tests/t_mtl_provider.m $(RUNTIME) runtime/kl_view.c \
               runtime/kl_view_mtl.m runtime/klepton.h runtime/kl_jni.h \
               runtime/kl_view.h runtime/kl_view_mtl.h tests/t_mtl_provider.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc $(shell pkg-config --cflags sdl3) -o $@ \
	  mains/m_slink.c tests/t_mtl_provider.m $(RUNTIME) runtime/kl_view.c \
	  runtime/kl_view_mtl.m \
	  $(LDLIBS) -framework Metal -framework QuartzCore -framework Foundation \
	  $(shell pkg-config --libs sdl3)

slink: build/m_slink
	./build/m_slink

# The VR build of the same app (steamlink-vr.apk, PLANNING §11.8). Same seven
# libraries for the 2D half, so SL-1 is the same gate; libvrlink_scene is the
# OpenXR NativeActivity and is NOT loaded by this target yet.
SLVRLIBS := steamlink-vr/lib/arm64-v8a
slink-vr: build/m_slink
	./build/m_slink $(SLVRLIBS)

# SL-2: onCreate's whole sequence, through nativeRunMain into SDL_main, with the
# app reaching its own renderer. Needs ANGLE (KL_GLFB=1) — the null GL driver
# stops at glCheckFramebufferStatus by design, so this is a real-GL gate.
#
# Expected: exit 0, "Created opengles2 renderer on android", "Initialized
# player", and a MESSAGEBOX reporting no streaming host on the network. That
# last line is SUCCESS, not failure: it is the app having got all the way to
# looking for a Steam machine to stream from. A picture needs a host (and then
# AMediaCodec); everything before that point is what this gate covers.
slink-main: build/m_slink
	KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1 ./build/m_slink $(SLVRLIBS)

# SL-4: the OTHER front door. The 2D configuration frontend — libshell + Qt6,
# which is what SteamLink.getMainSharedObject() actually names — instead of the
# streaming client. The point of it is that it has pixels of its own: the client
# draws nothing without a Steam host on the LAN (§11.11, zero swaps ever), and
# the shell draws Qt Widgets locally.
#
# VR APK only, and not for arbitrary reasons: the old APK ships Qt5 with the
# stock qtforandroid QPA (a whole QtAndroid JNI surface), the VR APK ships Qt6
# with Valve's own `qvirtual` QPA, which imports no JNI at all.
slink-shell: build/m_slink
	KL_SLINK_SHELL=1 KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1 ./build/m_slink $(SLVRLIBS)

# ...and the work list for it, which is the number that matters first: map and
# relocate all fourteen, print what is still unresolved, stop before init.
slink-shell-gap: build/m_slink
	KL_SLINK_SHELL=1 KL_GAP_ONLY=1 KL_NOFORK=1 ./build/m_slink $(SLVRLIBS)

build/t_variadic: tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(LDLIBS)

vatest: build/t_variadic
	./build/t_variadic

# kl_view.c is the KL_VIEW=1 interactive viewer (SDL3); only m_boot links it,
# so only this rule carries the pkg-config flags. kl_view.c compiles to a stub
# without SDL3 headers, but the link flags below assume pkg-config finds sdl3.
# tests/t_mtl_provider.m is the host stand-in for Compositor Services (P5.3):
# Objective-C because it has to *create* MTLTextures. Host-only and diagnostic —
# named here and never in RUNTIME_SHIP, which is why the shipping runtime stays
# plain C and takes an opaque texture pointer.
build/m_boot: mains/m_boot.c tests/t_mtl_provider.m $(RUNTIME) runtime/kl_view.c \
              runtime/kl_view_mtl.m runtime/klepton.h runtime/kl_jni.h \
              runtime/kl_view.h runtime/kl_view_mtl.h tests/t_mtl_provider.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc $(shell pkg-config --cflags sdl3) -o $@ \
	  mains/m_boot.c tests/t_mtl_provider.m $(RUNTIME) runtime/kl_view.c \
	  runtime/kl_view_mtl.m \
	  $(LDLIBS) -framework Metal -framework QuartzCore -framework Foundation \
	  $(shell pkg-config --libs sdl3)

boot: build/m_boot
	./build/m_boot $(LIBS)

# ---- guest payloads we build ourselves ----
#
# A guest we control, so S0.5 can be checked for *correct answers* rather than
# only for absence of crashes (tests/t_guest.c explains the design).
#
# The triple is aarch64-linux-gnu, not -android, and that is deliberate: modern
# NDKs reserve x18 as the ShadowCallStack platform register, so an Android target
# never allocates it and the payload would contain nothing to test. Beat Saber's
# libunity was built before that default changed, which is exactly why it has
# 14,536 x18 sites. -nostdlib keeps the payload free of libc so this stays a test
# of the loader and the veneer, not of the bionic shim.
NDK_CC := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android21-clang
GUEST_CFLAGS := --target=aarch64-unknown-linux-gnu -shared -O2 -fPIC -nostdlib \
                -ffreestanding -fuse-ld=lld

build/guest_torture.so: guest/torture.c guest/torture.h
	@mkdir -p build
	@test -x "$(NDK_CC)" || { echo "ANDROID_NDK_HOME is not set to an NDK with a darwin-x86_64 toolchain"; exit 1; }
	$(NDK_CC) $(GUEST_CFLAGS) -o $@ guest/torture.c

build/t_guest: tests/t_guest.c guest/torture.c guest/torture.h $(RUNTIME)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_guest.c guest/torture.c $(RUNTIME) $(LDLIBS)

guest: build/t_guest build/guest_torture.so
	./build/t_guest build/guest_torture.so

# S0.5 — the x18 decoder, checked against objdump over every guest library.
# Deliberately not linked against the runtime: it reads ELF files, it does not
# load them, so a decoder bug cannot hide behind a working loader.
build/t_x18: tests/t_x18.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_x18.c runtime/kl_x18.c runtime/kl_env.c

x18: build/t_x18
	python3 tools/check_x18.py build/t_x18 $(LIBS)/libunity.so $(LIBS)/libil2cpp.so \
	  $(LIBS)/libunityopus.so $(LIBS)/libmain.so $(LIBS)/lib_burst_generated.so

# The same decoder check against the second target. Kept separate from `x18`
# because it proves a different thing: Steam Link is a different toolchain
# (Valve's clang, BoringSSL, SDL3) and it is what found the data-in-.text class
# — 1059 of libmain.so's 1080 apparent x18 sites are BoringSSL constants.
SLLIBS := steamlink-android/lib/arm64-v8a
x18-slink: build/t_x18
	python3 tools/check_x18.py build/t_x18 $(SLLIBS)/libmain.so $(SLLIBS)/libSDL3.so \
	  $(SLLIBS)/libSDL3_ttf.so $(SLLIBS)/libSDL3_image.so $(SLLIBS)/libc++_shared.so
	python3 tools/check_x18.py build/t_x18 $(SLVRLIBS)/libmain.so $(SLVRLIBS)/libSDL3.so \
	  $(SLVRLIBS)/libvrlink_scene.so $(SLVRLIBS)/libopenxr_loader.so

# kl_jni_slots.h is checked in; regenerate only when bumping the NDK.
jnislots:
	python3 tools/gen_jni_slots.py > runtime/kl_jni_slots.h

build/t_il2cpp: tests/t_il2cpp.c $(RUNTIME) runtime/klepton.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_il2cpp.c $(RUNTIME) $(LDLIBS)

il2cpp: build/t_il2cpp
	./build/t_il2cpp

# Full regression sweep — run this first when picking work back up.
#
# Each test writes to a log and is checked BEFORE the log is filtered. Piping a
# test straight into tail/grep would hand make the filter's exit status instead
# of the test's, so a failing test would leave the sweep green.
check: build/t_opus build/t_variadic build/t_load build/t_il2cpp build/m_boot build/t_haptics
	@echo "=== variadic ABI ===" && ./build/t_variadic
	@./build/t_haptics > build/haptics.log 2>&1 || { cat build/haptics.log; exit 1; }
	@head -3 build/haptics.log && tail -1 build/haptics.log
	@echo "=== opus roundtrip ===" && ./build/t_opus $(LIBS)/libunityopus.so > build/opus.log && tail -3 build/opus.log
	@echo "=== all guest libraries ===" && for f in libmain lib_burst_generated libunityopus libunity libil2cpp; do \
	  printf '%-24s' $$f; ./build/t_load $(LIBS)/$$f.so 2>/dev/null | grep -E '^  imports:'; done
	@echo "=== S0.5 x18 veneers ===" && for f in libunity libil2cpp; do \
	  printf '%-24s' $$f; ./build/t_load $(LIBS)/$$f.so 2>/dev/null | grep -E '^  x18 sites:'; done
	@echo "  (the 2 refused sites are libunity's br x18 jump tables — see PLANNING S0.5;"
	@echo "   'make x18' is the exhaustive decoder check against objdump, run it after"
	@echo "   any change to runtime/kl_x18.c)"
	@if [ -x "$(NDK_CC)" ]; then echo "=== guest differential (S0.5) ===" && \
	   $(MAKE) -s guest | grep -E 'x18 sites|identical to the host|lost '; \
	 else echo "=== guest differential: SKIPPED (set ANDROID_NDK_HOME) ==="; fi
	@echo "=== il2cpp runtime ===" && ./build/t_il2cpp $(LIBS)/libil2cpp.so beatsaber/assets/bin/Data/Managed > build/il2cpp.log && tail -4 build/il2cpp.log
	@echo "=== guest entry (JNI_OnLoad, initJni) ===" && ./build/m_boot $(LIBS) > build/boot.log 2>/dev/null && \
	  grep -E 'guard verified|DLC enumeration|JNI_OnLoad returned|registered com|load returned|natives registered:|ids requested:|M3 EXIT|M4 \(partial\)' build/boot.log

# ---- M1b / the visionOS port (PLANNING §12) ----
#
# klepton-ld translates a guest ELF .so into a Mach-O dylib so dyld — not our
# mmap loader — maps the guest text. It links runtime/kl_x18.c because the S0.5
# decoder is the authority on what an x18 site is; a bit-field guess reports
# 2158 sites in libunityopus.so where the decoder correctly reports 0.
build/klepton-ld: tools/klepton_ld.c runtime/kl_x18.c runtime/kl_x18.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/klepton_ld.c runtime/kl_x18.c

# P1 — the same t_opus roundtrip that M1a passes, through the translated dylib.
# t_opus picks its loader from the file's magic, so this is one test over two
# loaders rather than two tests. arm64 macOS requires at least an ad-hoc
# signature before dyld will map anything.
.PHONY: p1
p1: build/klepton-ld build/t_opus
	@./build/klepton-ld $(LIBS)/libunityopus.so -o build/libunityopus.dylib
	@codesign -s - -f build/libunityopus.dylib 2>/dev/null
	@./build/t_opus $(PWD)/build/libunityopus.dylib | tail -4

# P2 — the same dylib under the visionOS Simulator's dyld, which is where a
# malformed hand-emitted Mach-O gets caught (§4, rung 2). Needs a booted
# visionOS simulator; XRSIM overrides which one.
XRSIM ?= $(shell xcrun simctl list devices booted 2>/dev/null | grep -o '[0-9A-F-]\{36\}' | head -1)
.PHONY: p2
p2: build/klepton-ld
	@test -n "$(XRSIM)" || { echo "no booted simulator — 'xcrun simctl boot <udid>' first"; exit 1; }
	@./build/klepton-ld $(LIBS)/libunityopus.so -o build/libunityopus-xrsim.dylib \
	   --platform visionossim --quiet
	@codesign -s - -f build/libunityopus-xrsim.dylib 2>/dev/null
	@clang -target arm64-apple-xros1.0-simulator \
	   -isysroot $$(xcrun --sdk xrsimulator --show-sdk-path) -arch arm64 \
	   -o build/dl_xrsim tools/dlprobe.c
	@xcrun simctl spawn $(XRSIM) $(PWD)/build/dl_xrsim $(PWD)/build/libunityopus-xrsim.dylib

# Translate every guest library. All five go through now that the x18 veneer
# pass runs offline; a library klepton-ld cannot take is reported rather than
# skipped silently, so a regression here reads as one.
.PHONY: dylibs
dylibs: build/klepton-ld
	@mkdir -p build/dylibs
	@for f in libmain lib_burst_generated libunityopus libunity libil2cpp; do \
	  if ./build/klepton-ld $(LIBS)/$$f.so -o build/dylibs/$$f.dylib \
	       --platform $(KL_PLATFORM) --quiet 2>build/dylibs/$$f.err; then \
	    codesign -s - -f build/dylibs/$$f.dylib 2>/dev/null; \
	    printf '  %-22s translated\n' $$f; \
	  else \
	    rm -f build/dylibs/$$f.dylib; \
	    printf '  %-22s REFUSED — %s\n' $$f "$$(sed 's/^klepton-ld: //' build/dylibs/$$f.err | head -1)"; \
	  fi; done
KL_PLATFORM ?= macos

# m_boot over the translated libraries. KL_DYLIB_DIR makes every guest-library
# load prefer a translation when one exists, so this exercises kl_load_dylib
# inside the real boot chain — the image registry, the DT_NEEDED walk and the
# guest's own dlopen of libunity — rather than in t_opus's single-image case.
# Mixed until the x18 pass lands; `make dylibs` prints which half is which.
.PHONY: bootdylib
bootdylib: dylibs build/m_boot
	@KL_DYLIB_DIR=$(PWD)/build/dylibs ./build/m_boot $(LIBS) > build/bootdylib.log 2>&1; \
	  echo "  exit=$$?"; \
	  grep -aE 'loaded as a translated dylib|guard verified|natives registered:|ids requested:|M3 EXIT' \
	    build/bootdylib.log
	@echo "  NOTE: this reaches libmain and libunity. libil2cpp arrives only in the"
	@echo "  lifecycle ('make bootdylib-life'), and libunityopus/lib_burst_generated"
	@echo "  are not dlopen'd by either — 'make loaddylib' covers those on their own."

# The lifecycle over translated dylibs: this is the run that pulls libil2cpp in
# and executes all 17,617 veneers out of a dyld-mapped, read-only __TEXT. Must
# match the ELF baseline — exit 0 and 26 swaps under the null driver.
.PHONY: bootdylib-life
bootdylib-life: dylibs build/m_boot
	@KL_DYLIB_DIR=$(PWD)/build/dylibs KL_FRAMES=30 KL_ALARM=60 KL_LIFECYCLE=1 \
	  script -q /dev/null timeout 300 ./build/m_boot $(LIBS) > build/bootdylib-life.log 2>&1; \
	  echo "  exit=$$?"; \
	  grep -aE 'loaded as a translated dylib|eglSwapBuffers:' build/bootdylib-life.log

# Load each translated library through kl_load_dylib on its own, since the boot
# chain does not reach all of them. Import counts must match `make check`'s.
.PHONY: loaddylib
loaddylib: dylibs build/t_load
	@for f in libmain lib_burst_generated libunityopus libunity libil2cpp; do \
	  test -f build/dylibs/$$f.dylib || continue; \
	  printf '  %-22s' $$f; \
	  KL_DYLIB_DIR=$(PWD)/build/dylibs ./build/t_load $(LIBS)/$$f.so 2>&1 \
	    | grep -E '^  imports:' | tr -d '\n'; echo; done

# ---- P4 — the runtime builds for visionOS (PLANNING §12.4) ----
#
# Two gates, and the second is the one that carries the information. Compiling
# proves the headers are there; *linking* proves every symbol the runtime
# references is present in the SDK, which is where an API that is declared but
# absent — or present on macOS and nowhere else — actually shows up.
# -all_load forces every object in and -undefined error refuses to defer, so a
# missing symbol inside a function nothing calls yet still fails the build.
#
# Both platforms are built, because they diverge: xrsimulator is a macOS-kernel
# host with an iOS-shaped SDK, so a header present only there would pass the
# simulator and fail the device.
XROS_SDK  := $(shell xcrun --sdk xros --show-sdk-path)
XRSIM_SDK := $(shell xcrun --sdk xrsimulator --show-sdk-path)
XROS_CFLAGS := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64

.PHONY: xros xros-device xros-sim
xros: xros-device xros-sim build/Klepton.xcframework
	@echo "  P4 gate: the shipping runtime compiles and links for both visionOS platforms."

# The archives are real targets with real prerequisites, not side effects of a
# .PHONY rule. That distinction cost a debugging pass: with only a phony rule
# producing it, an edited kl_image.c left a stale libklepton.a in place, the
# xcframework was built from it, and the app in the simulator silently ran the
# *previous* loader — which reads as a bug in the app rather than in the build.
build/xros/libklepton.a: $(RUNTIME_SHIP)
	@mkdir -p build/xros
	@rm -f $@
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xros/$$(basename $$s | sed 's/\.[cS]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) \
	    -c $$s -o $$o || exit 1; done
	@ar rcs $@ build/xros/*.o

build/xrsim/libklepton.a: $(RUNTIME_SHIP)
	@mkdir -p build/xrsim
	@rm -f $@
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xrsim/$$(basename $$s | sed 's/\.[cS]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) \
	    -c $$s -o $$o || exit 1; done
	@ar rcs $@ build/xrsim/*.o

# The gate: link each archive into a dylib with -all_load, so a symbol that is
# declared in a header but absent from the platform fails the build even inside
# a function nothing calls yet.
build/xros/libklepton.dylib: build/xros/libklepton.a
	@$(CC) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -framework AudioToolbox \
	   -install_name @rpath/libklepton.dylib

build/xrsim/libklepton.dylib: build/xrsim/libklepton.a
	@$(CC) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -framework AudioToolbox \
	   -install_name @rpath/libklepton.dylib

xros-device: build/xros/libklepton.dylib
	@printf '  xros       '; xcrun vtool -show-build $< | grep -E 'platform' | tr -d '\n'
	@printf '  %s\n' "$$(ls -l $< | awk '{print $$5" bytes"}')"

xros-sim: build/xrsim/libklepton.dylib
	@printf '  xrsim      '; xcrun vtool -show-build $< | grep -E 'platform' | tr -d '\n'
	@printf '  %s\n' "$$(ls -l $< | awk '{print $$5" bytes"}')"

# The two archives as one XCFramework, so the app target links the right slice
# without a per-platform search path. Xcode resolves the slice from the
# destination; getting that wrong otherwise fails as a confusing arch mismatch.
#
# The headers are prerequisites too, and that is not pedantry. `-headers runtime`
# COPIES them into the xcframework, and the app compiles against the copy — so
# with only the archives listed, a header-only change (a new prototype, say) never
# reaches the app and the build fails with "call to undeclared function" naming a
# function that is plainly declared in runtime/. This is the same stale-artifact
# trap visionos/README.md records for libklepton.a; the fix there stopped at the
# archives and left the headers behind it.
build/Klepton.xcframework: build/xros/libklepton.a build/xrsim/libklepton.a \
                           $(wildcard runtime/*.h)
	@rm -rf $@
	@xcodebuild -create-xcframework \
	   -library build/xros/libklepton.a -headers runtime \
	   -library build/xrsim/libklepton.a -headers runtime \
	   -output $@ > /dev/null
	@echo "  build/Klepton.xcframework"

# ANGLE for visionOS. The vendored checkout's gn has no xros target and does not
# need one: build for iOS — which already enables the Metal backend — and rewrite
# LC_BUILD_VERSION afterwards. Confirmed prior art (ALVR ships a Rust dylib this
# way); §12.1(1). ANGLE emits ios_framework_bundle on iOS, so the output is
# already the .framework packaging §4.0.1 wants.
.PHONY: angle-ios angle-ios-sim angle-xros
angle-ios: angle-fetch
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gn gen out/ios --args='is_debug=false target_os="ios" target_cpu="arm64" \
	    target_environment="device" ios_enable_code_signing=false \
	    angle_enable_vulkan=false angle_enable_swiftshader=false' && \
	  autoninja -C out/ios libEGL libGLESv2

# The simulator slice. Same trick one platform over: an iOS *simulator* build
# vtool'd to visionossim. Needed because the simulator is the fast development
# loop for P5.3 — a device round trip is minutes, the simulator is seconds — and
# an xcframework with no matching slice fails as an arch mismatch rather than as
# a missing slice.
angle-ios-sim: angle-fetch
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gn gen out/ios-sim --args='is_debug=false target_os="ios" target_cpu="arm64" \
	    target_environment="simulator" ios_enable_code_signing=false \
	    angle_enable_vulkan=false angle_enable_swiftshader=false' && \
	  autoninja -C out/ios-sim libEGL libGLESv2

# The retarget itself is a script (tools/angle_retarget.sh) — it rewrites the
# Info.plist as well as LC_BUILD_VERSION, and the header there says why.
angle-xros: angle-ios
	@./tools/angle_retarget.sh ios xros visionos XROS

.PHONY: angle-xrsim
angle-xrsim: angle-ios-sim
	@./tools/angle_retarget.sh ios-sim xrsim visionossim XRSimulator

# ---- graphics spikes (host-only, not part of `make check`) ----
# S0.7/S0.8 probe Apple's desktop GL; S0.9 probes ANGLE. None of them link the
# runtime — they answer questions about the host, not about the shim.
build/s07_glfb: spikes/s07_glfb.c
	$(CC) $(CFLAGS) -o $@ $< -framework OpenGL -lz
build/s08_glsl: spikes/s08_glsl.c
	$(CC) $(CFLAGS) -o $@ $< -framework OpenGL
build/s09_angle: spikes/s09_angle.c
	$(CC) $(CFLAGS) -o $@ $<
build/s10_shared: spikes/s10_shared.c
	$(CC) $(CFLAGS) -o $@ $<

# S1.1 — the P5 interop primitive: ANGLE rendering into an MTLTexture we own.
# Objective-C because the spike has to *create* the MTLTexture; the shipping
# path never does — it receives one from Swift as an opaque pointer, which is
# why kl_glfb stays plain C (PLANNING §12.6).
build/s11_mtltex: spikes/s11_mtltex.m
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< -framework Metal -framework Foundation

.PHONY: mtltex
mtltex: build/s11_mtltex
	@./build/s11_mtltex

# S1.2 — foveation: does ANGLE rasterize through an MTLRasterizationRateMap we
# own? The gate for the whole variable-rate arc, and the thing that says
# angle-patches/klepton.patch's Metal-backend half actually engages. Needs the
# PATCHED ANGLE (`make angle-debug`); it names that as the failure if the entry
# point is missing rather than reporting the design as broken.
build/s12_vrr: spikes/s12_vrr.m
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< -framework Metal -framework Foundation

.PHONY: vrr
vrr: build/s12_vrr
	@./build/s12_vrr

# Timewarp — the composite/reprojection pass, checked without a headset: the
# matrices, and that the shared shader actually compiles. Separate from `make
# check` on purpose; it needs Metal's compiler service and the gate should not
# take a dependency on that. See tests/t_reproject.m.
build/t_reproject: tests/t_reproject.m runtime/kl_reproject.c runtime/kl_reproject.h \
                   runtime/kl_env.c runtime/kl_ovrp.h
	$(CC) $(CFLAGS) -fobjc-arc -Iruntime -o $@ $< runtime/kl_reproject.c \
	  runtime/kl_env.c -framework Metal -framework Foundation

.PHONY: reproject
reproject: build/t_reproject
	@./build/t_reproject

# M8 — the haptics seam's model, with no guest and no headset: the three
# OVRPlugin entry points AS THE GUEST RESOLVES THEM (through kl_ovrp_sym, so
# the struct returns go through the real sret thunk), the queue behind them,
# and the pulses a frontend pulls. Fast and deterministic; part of `check`.
build/t_haptics: tests/t_haptics.c $(RUNTIME) runtime/kl_ovrp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/t_haptics.c $(RUNTIME) $(LDLIBS)

.PHONY: haptics
haptics: build/t_haptics
	@./build/t_haptics

# A *vendored debug build* of ANGLE lives in vendor/ (gitignored) — the Metal
# backend can be stepped into, which is what the AGX-abort investigation needs.
# Every loader (kl_glfb.c and the s09/s10/s11/s13 spikes) uses vendor/out/Debug
# and nothing else: ours is PATCHED (angle-patches/), so falling back to another
# ANGLE on the machine would swap the thing under test for a different one.
# KL_ANGLE_DIR still overrides, deliberately.
#
# vendor/ is a shallow ANGLE checkout with depot_tools in vendor/depot_tools.
# `make angle-fetch` produces it; this target only builds. One-time setup cost:
# checkout ~12 GB, build ~30-60 min.
.PHONY: angle-debug
angle-debug: angle-fetch
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gn gen out/Debug --args='is_debug=true target_cpu="arm64"' && \
	  autoninja -C out/Debug libEGL libGLESv2

# ---- vendor/ — the ANGLE checkout, which we MODIFY ----
#
# ANGLE is not a submodule and cannot be one: DEPS pulls ~40 further
# repositories plus CIPD and GCS blobs and pins them itself, which is gclient's
# job and not git's. So the checkout is a make step instead.
#
# It has to be safe to re-run against a tree with local work in it, because
# there IS local work in it — Klepton raises ANGLE's Metal sampler limit today,
# and variable rasterization rate rendering lands in the same backend. Four
# things make that safe:
#
#   * the solution is "managed": False, so `gclient sync` resolves DEPS and
#     never touches ANGLE's own git. Managed — the default, and what a
#     hand-written `fetch angle` gives you — resets the checkout to the pinned
#     revision, and uncommitted work is simply gone.
#   * DEPOT_TOOLS_UPDATE=0, or depot_tools silently updates itself past its
#     pin and the two halves of the toolchain drift apart.
#   * the ANGLE clone happens ONCE. If vendor/.git exists this target never
#     writes to it: no fetch, no checkout, no reset.
#   * the sync is stamped, so a healthy tree is a fast no-op rather than a
#     multi-minute re-resolve. `make angle-sync` forces it.
#
# Our delta lives in angle-patches/klepton.patch, which IS tracked in this
# repo. It is applied to a fresh checkout and re-exported by `make angle-save`.
# That file, not the gitignored vendor/ tree, is what makes the modification
# reproducible — treat an unsaved change in vendor/ as unbacked-up.
#
# The `.` solution name is load-bearing: it puts ANGLE at the vendor root,
# which is what every path here assumes. depot_tools' own `fetch angle` writes
# "angle" instead and checks out into vendor/angle/.
ANGLE_URL    := https://chromium.googlesource.com/angle/angle.git
ANGLE_COMMIT := 25e721127e1c6c4c6fa0182b5c234b2c88971175
DEPOT_URL    := https://chromium.googlesource.com/chromium/tools/depot_tools.git
DEPOT_COMMIT := 6afa997717b2c0e1382e1465bedbe1a6855b9388
ANGLE_PATCH  := angle-patches/klepton.patch
ANGLE_STAMP  := vendor/.klepton-synced

.PHONY: angle-fetch angle-sync angle-save angle-status angle-all
angle-fetch: $(ANGLE_STAMP)

# Everything ANGLE, in one command: pull + patch + all three slices. This is
# the build-it-once target for someone who is not developing ANGLE itself.
# Each half is idempotent, so re-running is cheap once it is current.
angle-all: angle-debug angle-xros angle-xrsim
	@echo "  ANGLE: host (out/Debug), visionOS device (out/xros), simulator (out/xrsim)"

$(ANGLE_STAMP):
	@mkdir -p vendor
	@if [ -d vendor/.git ]; then \
	  echo "  [angle] vendor/ is already a checkout — leaving its git untouched"; \
	else \
	  echo "  [angle] cloning ANGLE @ $(ANGLE_COMMIT) (this is the ~12 GB step)"; \
	  git -C vendor init -q && git -C vendor remote add origin $(ANGLE_URL) && \
	  { git -C vendor fetch -q --depth 1 origin $(ANGLE_COMMIT) || \
	    git -C vendor fetch -q origin; } && \
	  git -C vendor checkout -q -B klepton $(ANGLE_COMMIT) || exit 1; \
	  if [ -s $(ANGLE_PATCH) ]; then \
	    git -C vendor apply $(PWD)/$(ANGLE_PATCH) || exit 1; \
	    echo "  [angle] applied $(ANGLE_PATCH)"; \
	  fi; \
	fi
	@if [ ! -d vendor/depot_tools/.git ]; then \
	  echo "  [angle] cloning depot_tools @ $(DEPOT_COMMIT)"; \
	  git clone -q $(DEPOT_URL) vendor/depot_tools && \
	  git -C vendor/depot_tools checkout -q $(DEPOT_COMMIT) || exit 1; \
	fi
	@printf '%s\n' 'solutions = [' '  {' '    "name": ".",' \
	  '    "url": "$(ANGLE_URL)",' '    "deps_file": "DEPS",' \
	  '    "managed": False,' '    "custom_vars": {},' '  },' ']' \
	  > vendor/.gclient
	@echo "  [angle] gclient sync (DEPS only — managed:False)"
	@cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gclient sync --no-history
	@date > $@
	@$(MAKE) -s angle-status

# Force the DEPS re-resolve the stamp otherwise skips — after bumping the pin,
# or when a build fails on something DEPS should have provided.
angle-sync:
	@rm -f $(ANGLE_STAMP)
	@$(MAKE) -s angle-fetch

# Export vendor/'s delta against the pin into the tracked patch. This is the
# "do not lose it" button: everything in vendor/ is gitignored, so an ANGLE
# change that has not been through here exists in exactly one place on one
# machine. Committed-on-top and uncommitted changes are captured together, as
# one diff against the pin — granularity lives in vendor/'s own branch.
angle-save:
	@test -d vendor/.git || { echo "  vendor/ not checked out — 'make angle-fetch'"; exit 1; }
	@mkdir -p angle-patches
	@git -C vendor diff $(ANGLE_COMMIT) > $(ANGLE_PATCH)
	@printf '  wrote %s — %s file(s), %s lines\n' "$(ANGLE_PATCH)" \
	  "$$(git -C vendor diff --name-only $(ANGLE_COMMIT) | wc -l | tr -d ' ')" \
	  "$$(wc -l < $(ANGLE_PATCH) | tr -d ' ')"

angle-status:
	@if [ ! -d vendor/.git ]; then echo "  vendor/    absent — run 'make angle-fetch'"; else \
	  printf '  pin        %s\n' "$(ANGLE_COMMIT)"; \
	  printf '  HEAD       %s (%s)\n' "$$(git -C vendor rev-parse --short HEAD)" \
	    "$$(git -C vendor rev-parse --abbrev-ref HEAD)"; \
	  printf '  modified   %s file(s) vs the pin\n' \
	    "$$(git -C vendor diff --name-only $(ANGLE_COMMIT) | wc -l | tr -d ' ')"; \
	  if [ -f $(ANGLE_PATCH) ]; then \
	    if git -C vendor diff $(ANGLE_COMMIT) | diff -q - $(ANGLE_PATCH) >/dev/null 2>&1; \
	      then echo "  patch      $(ANGLE_PATCH) is up to date"; \
	      else echo "  patch      DIFFERS from vendor/ — run 'make angle-save'"; fi; \
	  else echo "  patch      $(ANGLE_PATCH) missing — run 'make angle-save'"; fi; \
	fi

# `make angle` / `make shared` exercise the vendored debug build — build it
# with `make angle-debug` first.
.PHONY: angle
angle: build/s09_angle
	@./build/s09_angle

# S1.0 — shared ANGLE contexts across threads. Runs the matrix that separates
# "shared contexts" from "concurrent use"; see the spike's header for the S10_*
# knobs.
#
# THE FAILURE IS INTERMITTENT, so every cell is repeated and what is reported is a
# rate, not a verdict. A single run of the failing configuration passes better than
# half the time — it did on the first attempt, and read as an exoneration. Set
# S10_RUNS to change the sample.
S10_RUNS ?= 20
.PHONY: shared
shared: build/s10_shared
	@printf '  %-46s %s\n' "configuration" "failures (of $(S10_RUNS))"; \
	 run() { desc="$$1"; shift; fail=0; \
	   for i in $$(seq 1 $(S10_RUNS)); do \
	     rc=$$(sh -c 'env "$$@" ./build/s10_shared >/dev/null 2>&1; echo $$?' \
	             _ "$$@" 2>/dev/null); \
	     [ "$$rc" = 0 ] || fail=$$((fail+1)); done; \
	   printf '  %-46s %d\n' "$$desc" "$$fail"; }; \
	 run "shared + concurrent (the KL_GLFB_SHARED shape)" S10_STAGE=7; \
	 run "shared, lock around compile+link" S10_STAGE=7 S10_SERIAL=2; \
	 run "shared, serialised entirely" S10_STAGE=7 S10_SERIAL=1; \
	 run "independent contexts, concurrent" S10_STAGE=7 S10_SHARE=0; \
	 run "one worker thread (control)" S10_STAGE=7 S10_THREADS=1; \
	 echo "  (expected: row 1 ~1 in 3, row 2 ~1 in 30, rows 3-5 clean)"

# The GL tracing trampoline's ABI contract (runtime/kl_gl_trace.S). It forwards an
# unknown signature, so nothing else can check it — and a trampoline that drops a
# register produces a wrong picture rather than a crash.
build/t_trace: tests/t_trace.c $(RUNTIME) runtime/klepton.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_trace.c $(RUNTIME) $(LDLIBS)

.PHONY: trace
trace: build/t_trace
	@./build/t_trace
