CC      := clang
# The Vulkan headers ship with the vendored MoltenVK (`make mvk`). The -I is
# unconditional and harmless when the directory is absent: kl_vulkan.c is
# guarded by __has_include, so a bare checkout still compiles and reports the
# missing dependency BY NAME when a guest asks for Vulkan, rather than failing
# the build for everyone who does not need it.
MVK_INC := -Ivendor-moltenvk/out/include
# Every header keeps its bare name on an #include line, wherever it lives: a
# source that moves between runtime/<area>/ directories then needs no include
# edits, and visionos/Sources/Klepton-Bridging-Header.h imports by bare name
# too. One -I per source directory is what buys that; gen_xcodeproj.py carries
# the same list as HEADER_SEARCH_PATHS.
RUNTIME_INC := -Iruntime -Iruntime/jni -Iruntime/libc -Iruntime/gfx -Iruntime/xr \
               -Iruntime/media -Iruntime/guest -Iruntime/diag
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 $(MVK_INC) $(RUNTIME_INC)
# VideoToolbox/CoreMedia/CoreVideo are the video decoder (kl_vtdec.c), and they
# are in the base LDLIBS rather than on one target because kl_vtdec is in
# RUNTIME_SHIP — everything that links the runtime needs them.
LDLIBS  := -lz -framework AudioToolbox \
           -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
           -framework CoreFoundation -framework AVFoundation -framework Foundation
# The host/ship split is a source-list boundary, not a runtime getenv.
#
# RUNTIME_SHIP is everything that goes into the visionOS app bundle. It is the
# list `make xros` builds against the xrOS SDK, so anything added here has to
# compile for the device — which is the point of keeping it separate rather
# than discovering the divergence at port time.
#
# RUNTIME_DIAG is host-only instrumentation: the sampling profiler, the
# managed-side probe and the metadata dump. Nothing in RUNTIME_SHIP includes
# their headers (only
# tests/m_boot.c does), so the boundary holds by construction rather than by
# discipline. runtime/gfx/kl_view.c (SDL viewer) is host-only too and is named by
# the m_boot rule alone.
# The synthetic JavaVM/JNIEnv. kl_jni.c is the mechanism (registries, dispatch,
# id interning); each kl_jni_<family>.c owns one block of the guest's Java
# classes and exports its binding table through runtime/jni/kl_jni_int.h.
RUNTIME_JNI := runtime/kl_jni.c \
           runtime/jni/kl_jni_lang.c runtime/jni/kl_jni_android.c \
           runtime/jni/kl_jni_looper.c runtime/jni/kl_jni_display.c \
           runtime/jni/kl_jni_bridge.c runtime/jni/kl_jni_window.c \
           runtime/jni/kl_jni_net.c runtime/jni/kl_jni_softinput.c \
           runtime/jni/kl_jni_services.c runtime/jni/kl_jni_io.c \
           runtime/jni/kl_jni_prefs.c runtime/jni/kl_jni_sdl.c \
           runtime/jni/kl_jni_ue4.c runtime/jni/kl_jni_jkxr.c

RUNTIME_SHIP := runtime/kl_env.c runtime/kl_image.c runtime/kl_stub_cells.S runtime/libc/kl_shim.c runtime/libc/kl_va.c \
           runtime/libc/kl_va_handlers.c runtime/libc/kl_va_thunks.S \
           runtime/libc/kl_libc.c runtime/libc/kl_libc_slink.c runtime/libc/kl_pthread.c runtime/kl_dl.c \
           runtime/guest/kl_ndk.c runtime/kl_x18.c runtime/kl_target.c \
           $(RUNTIME_JNI) \
           runtime/gfx/kl_egl.c runtime/media/kl_opensl.c runtime/media/kl_audio.c runtime/xr/kl_ovrp.c \
           runtime/xr/kl_ovrp_sret.S runtime/gfx/kl_reproject.c runtime/gfx/kl_present.c \
           runtime/xr/kl_ovrplat.c runtime/xr/kl_openxr.c runtime/media/kl_mediandk.c runtime/media/kl_vtdec.c runtime/media/kl_avdec.m \
           runtime/gfx/kl_vulkan.c \
           runtime/guest/kl_nativeactivity.c runtime/guest/kl_slink.c runtime/guest/kl_ue4.c \
           runtime/guest/kl_jkxr.c runtime/guest/kl_driver.c \
           runtime/media/kl_aaudio.c \
           runtime/gfx/kl_glfb.c runtime/gfx/kl_cvmtl.m runtime/gfx/kl_gl_trace.S runtime/gfx/kl_gl_lock.S \
           runtime/guest/kl_mono.c \
           runtime/guest/kl_il2cpp.c runtime/kl_fault.c runtime/guest/kl_guestpatch.c \
           runtime/guest/kl_guestpoke.c runtime/kl_cacerts.c runtime/media/kl_phonon_hrtf.S
RUNTIME_DIAG := runtime/diag/kl_sample.c runtime/diag/kl_mprobe.c runtime/diag/kl_metadump.c
RUNTIME := $(RUNTIME_SHIP) $(RUNTIME_DIAG)

# ...and every header they include, as a PREREQUISITE (never as a compiler
# input — a .h on the command line is compiled as a header, not included).
#
# A wildcard rather than a list, because the two that matter most are GENERATED:
# kl_libc_table.h and kl_target_table.h are written by tools, included by
# kl_shim.c and kl_target.c, and were prerequisites of nothing. That is not a
# missed rebuild, it is a missed FAILURE — regenerating the libc table with a
# name Darwin does not declare (SUPERHOT's `sched_getcpu`) left a table that
# cannot compile, make rebuilt nothing because no prerequisite had changed, and
# the whole of `make check` then passed against the PREVIOUS binary. Over-
# triggering costs a 20-second recompile; under-triggering costs a green gate
# that measured a build nobody has.
# kl_phonon_hrtf.S .incbins the SOFA blob, so the data file is a prerequisite
# of everything too — same missed-FAILURE argument as the generated tables.
RUNTIME_ALL_HDRS := $(wildcard runtime/*.h) $(wildcard runtime/*/*.h)
RUNTIME_HDRS := $(RUNTIME_ALL_HDRS) $(wildcard tests/*.h) \
                runtime/data/phonon_hrtf_cipic_124.sofa

.PHONY: all test clean check load vatest il2cpp boot jnislots x18 guest
all: build/t_opus

build/t_opus: tests/t_opus.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_opus.c $(RUNTIME) $(LDLIBS)

test: build/t_opus
	./build/t_opus $(LIBS)/libunityopus.so

# Safe to run: the guest's saves, PlayerPrefs and Steam Link pairing live in
# ~/Library/Application Support/Klepton/userdata/<guest>, NOT in build/. They
# used to be build/android-files and build/steamlink-files, so this target cost
# a Beat Saber first-run setup and a Steam Link re-pairing every time it ran.
clean:
	rm -rf build

# What a GUEST swap actually requires — the answer is "much less than `clean`",
# and `clean` is the expensive way to get it.
#
# Nothing caches a translated guest on the ELF path: kl_image maps, relocates,
# rewrites TLS and veneers x18 at LOAD time, every run, so a new APK is picked
# up with no step at all. Two things do persist across a swap:
#
#   build/dylibs/  klepton-ld output, and the ONE genuinely stale artefact —
#                  Mach-O translations of the PREVIOUS guest, which the dylib
#                  gates (make dylibs / loaddylib / bootdylib / bootdylib-life)
#                  and the visionOS app would otherwise keep using. Nothing
#                  names the guest version in them, so a stale one is silently
#                  the wrong library rather than a missing one.
#
#   kl_libc_table.h  generated from the guests' undefined symbols. Now a UNION
#                  over every unpacked tree, so a swap only ever ADDS to it —
#                  but a genuinely new title brings imports nothing forwards yet.
#
# Not here on purpose: visionos/run.sh's staging stamp is keyed on the APK's
# size and mtime, so it re-stages on its own; and the userdata above must
# survive, which is the whole reason it moved out of build/.
.PHONY: guestswap
guestswap:
	@rm -rf build/dylibs
	@echo "  removed build/dylibs (previous guest's Mach-O translations)"
	@python3 tools/gen_libc_table.py
	@$(MAKE) --no-print-directory guestlibs
	@echo "  userdata untouched: $$HOME/Library/Application Support/Klepton/userdata"
	@echo "  next: 'make check' (rebuilds what the table changed), then 'make dylibs'"
	@echo "        if you use the dylib or visionOS paths."

build/t_load: tests/t_load.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_load.c $(RUNTIME) $(LDLIBS)

# Which guest the host gates run against. `make check TARGET=superhot` points
# every one of them at another title's tree — the libraries, the assets and the
# APK together, which is the pairing that matters (a run with one version's
# libraries and another's assets reports itself as corrupt game data). The names
# are visionos/targets.py's, and the tree layout is the convention every unpacked
# APK here follows, so this needs no Python.
#
# LIBS stays overridable on its own because the Steam Link gates set it to a
# path directly.
TARGET ?= beatsaber
LIBS ?= $(TARGET)/lib/arm64-v8a

# The guest library set is DISCOVERED, not listed. It is a property of the APK,
# and pinning it means every version swap silently degrades: the 2019.4 Beat
# Saber build had lib_burst_generated and libunityopus, 1.6.0 (Unity 2018.4) has
# neither and adds libvrintegrationloader, and the stale list took the gates
# down with it while also starving tools/gen_libc_table.py of the twelve imports
# the new libraries actually need. Globbing costs nothing and means pointing
# LIBS at another Unity title just works.
#
# A `.so` IS NOT NECESSARILY AN ELF, and the discovery has to say so.
# Some code injections ship config files this way.
guest_elf_libs = $(shell for f in $(1)/*.so; do \
                   head -c4 "$$f" 2>/dev/null | grep -q ELF && echo "$$f"; done)
GUEST_SOS = $(basename $(notdir $(call guest_elf_libs,$(LIBS))))

# The exclusions are the libraries we REPLACE rather than translate
GUEST_REPLACED := libOVRPlugin libovrplatformloader libvrapi

# ...and these are not part of the APPLICATION at all.
GUEST_EXCLUDED := libfrda libscript

GUEST_LIBS = $(filter-out $(GUEST_REPLACED) $(GUEST_EXCLUDED),$(GUEST_SOS))

# The dyld / AMFI acceptance probes (P2, P3) do not care WHICH guest library
# they carry — they ask whether a hand-emitted Mach-O is accepted at all. P1
# does care, because it runs opus through the translated dylib and so proves
# guest CODE executes, not just that it maps. Prefer libunityopus where the
# title has one and fall back to libmain, which every Unity APK ships.
DYLIB_PROBE_LIB = $(if $(filter libunityopus,$(GUEST_LIBS)),libunityopus,libmain)

load: build/t_load
	@for f in $(GUEST_LIBS); do \
	  ./build/t_load $(LIBS)/$$f.so || true; echo; done

# What the discovery above actually resolved to, for when a gate's numbers move
# after a version swap and the first question is "against which libraries?".
guestlibs:
	@echo "LIBS = $(LIBS)"
	@echo "translated: $(GUEST_LIBS)"
	@echo "replaced:   $(filter $(GUEST_REPLACED),$(GUEST_SOS))"
	@echo "excluded:   $(filter $(GUEST_EXCLUDED),$(GUEST_SOS))"
	@echo "not ELF:    $(filter-out $(GUEST_SOS),\
	         $(basename $(notdir $(wildcard $(LIBS)/*.so))))"

# ...and the same answer with nothing around it, for a caller rather than a
# reader. visionos/targets.py asks for it with LIBS pointing at the target's own
# srcdir, so the app bundle embeds exactly what this tree would translate — the
# alternative is a pinned list in a second file, which is how 1.40 ended up with
# an app carrying five of its eleven libraries and no libOculusXRPlugin.
.PHONY: guestlibs-list
guestlibs-list:
	@echo $(GUEST_LIBS)

# ---- Steam Link ----
#
# One target row like any other (`./build/m_boot steamlink-vr`), and one binary:
# the front door is a KNOB, not a build. All three doors are the same guest, the
# same profile and the same APK — steamlink-vr.apk, which carries libmain and the
# SDL3 chain as well as libvrlink_scene, so the streaming client is reachable on
# it too. steamlink-android/ is the older Qt5 build and is kept only for the ELF
# scans below.
SLVRLIBS := steamlink-vr/lib/arm64-v8a
SLINK := steamlink-vr

# SL-1: the CLIENT chain binds and libSDL3's JNI_OnLoad runs. The VR door's
# equivalent is `slink-vr-scene` — with no KL_SLINK_MAIN it stops at onCreate,
# which is that door's chain gate.
slink: build/m_boot
	./build/m_boot $(SLINK)

# onCreate's whole sequence, through nativeRunMain into SDL_main, with the
# app reaching its own renderer. Needs ANGLE (KL_GLFB=1) — the null GL driver
# stops at glCheckFramebufferStatus by design, so this is a real-GL gate.
#
# Expected: exit 0, "Created opengles2 renderer on android", "Initialized
# player", and a MESSAGEBOX reporting no streaming host on the network. That
# last line is SUCCESS, not failure: it is the app having got all the way to
# looking for a Steam machine to stream from. A picture needs a host (and then
# AMediaCodec); everything before that point is what this gate covers.
slink-main: build/m_boot
	KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1 ./build/m_boot $(SLINK)

# The OTHER front door: the 2D configuration frontend — libshell + Qt6,
# which is what SteamLink.getMainSharedObject() actually names — instead of the
# streaming client. The point of it is that it has pixels of its own: the client
# draws nothing without a Steam host on the LAN (zero swaps ever), and
# the shell draws Qt Widgets locally.
#
# VR APK only, and not for arbitrary reasons: the old APK ships Qt5 with the
# stock qtforandroid QPA (a whole QtAndroid JNI surface), the VR APK ships Qt6
# with Valve's own `qvirtual` QPA, which imports no JNI at all.
# KL_SLINK_HANDOFF=0 because this gate measures the SHELL. Credentials persist
# across runs, so a machine that has paired once can reach startVRLink without
# anyone clicking anything, and the default handoff would re-exec this recipe
# into the VR front door — which is a different measurement wearing this one's
# name. `./build_run_host.sh steamlink-vr --shell --view` is where the handoff
# belongs.
slink-shell: build/m_boot
	KL_SLINK_SHELL=1 KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1 KL_SLINK_HANDOFF=0 \
	  ./build/m_boot $(SLINK)

# ...and the work list for it, which is the number that matters first: map and
# relocate all fourteen, print what is still unresolved, stop before init.
slink-shell-gap: build/m_boot
	KL_SLINK_SHELL=1 KL_GAP_ONLY=1 KL_NOFORK=1 ./build/m_boot $(SLINK)

# The THIRD front door: libvrlink_scene.so, the OpenXR NativeActivity.
# Not SDL3 at all, and the chain is ONE guest library: its DT_NEEDED is entirely
# Android system libraries we shim. libopenxr_loader.so is deliberately
# not loaded — it is REPLACED, the same call made for libOVRPlugin.
#
# The gap first, because that is the number that matters: 127 unresolved names,
# and they are the work list for the whole VR arc.
slink-vr-gap: build/m_boot
	KL_SLINK_VR=1 KL_GAP_ONLY=1 KL_NOFORK=1 ./build/m_boot $(SLINK)

# ...and the run: init arrays, then ANativeActivity_onCreate. Stops by name
# wherever the shim ends, which is the whole point.
slink-vr-scene: build/m_boot
	KL_SLINK_VR=1 KL_NOFORK=1 ./build/m_boot $(SLINK)

# ...and the whole thing running: the OpenXR boot sequence, the session
# state machine, and the frame loop, on ANGLE.
#
# KL_SLINK_SARGS is the 2D-to-VR handoff arriving: without it the app prints
# "No sArgs and release build panic" and exits before its first frame, which is
# correct behaviour and not a failure of ours. The value below is a SYNTHETIC
# one — the format, with no real host behind it — which is enough to carry the
# app through scene setup and into the stream, where it stops by name at
# AMediaCodec. Paste a real one from a pairing run to go
# further.
#
# Expected: the six-step boot, "Created session successfully", the state machine
# walking IDLE -> READY -> SYNCHRONIZED -> VISIBLE -> FOCUSED, two
# "[xr] eye N <- swapchain" lines from xrEndFrame, the decoder coming up
# ("[SVLDecoder] Finished decoder init"), and then a stop inside
# SVLDataLink::InitCrypt. Anything earlier is a regression.
#
# This target exits NON-ZERO on success, unlike the other three slink gates.
# The stop is not ours: it is the guest's own DebuggerBreak (`brk #1` at
# libvrlink_scene+0x15b798, so SIGTRAP rather than SIGABRT) after it rejects the
# SYNTHETIC token above. Same class of correct-behaviour stop as "No sArgs and
# release build panic": a fabricated credential is refused, which is what a
# credential is for.
#
# With a REAL sArgs, from the pairing -> handoff loop, it goes
# considerably further and exits 0 — the session is accepted, the
# UDP data link connects, the Steam host replies, AAudio comes up, and the run
# reaches the stream scene's WebView pre-flight. Measured from the disassembly
# so it is not re-derived: InitCrypt scans for SEVEN `~` and the int it logs is
# the whole sArgs length; the token begins with a FOUR-CHARACTER key identifier
# (`MID0`), which is why the failure message prints four characters — it is not
# a truncated print, and "make the token longer" was never the shape of it.
#
# Read the last "fault:"/"fatal:" line, not make's exit code.
KL_SLINK_SARGS ?= 192.168.1.50~10400~10400~0,0,1~~~~dGVzdA==
slink-vr-run: build/m_boot
	KL_SLINK_VR=1 KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1 KL_SLINK_WAIT=25 \
	  KL_SLINK_SARGS='$(KL_SLINK_SARGS)' ./build/m_boot $(SLINK)

# The video decode gate. kl_vtdec is the only piece of the video path
# that can be checked with no guest and no Steam host — an elementary stream in,
# frames out, both ends knowable — so it is where the assertions live. See
# tests/t_hevc.c for what each one catches. Seconds; in `make check`.
build/t_hevc: tests/t_hevc.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_hevc.c $(RUNTIME) $(LDLIBS)

hevc: build/t_hevc
	./build/t_hevc

# The video TEXTURE gate: a natively-decoded biplanar frame through
# CVMetalTextureCache + EGL_METAL_TEXTURE_ANGLE to sampled pixels, compared
# against the same picture through the BGRA path. This is what proves the
# vendored ANGLE still knows the private YCbCr formats — run it after any
# ANGLE re-pull or format-table change. Needs the PATCHED ANGLE
# (`make angle-debug`); NOT in `make check`, which must pass on a bare
# checkout with no vendor/.
build/t_vidtex: tests/t_vidtex.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_vidtex.c $(RUNTIME) $(LDLIBS)

.PHONY: vidtex
vidtex: build/t_vidtex
	./build/t_vidtex

# The OpenXR reference-space gate. The pose a runtime answers with is not
# visible from anywhere else — every call succeeds and the picture is correct
# either way — so the one thing that could see the eye-to-head carrying the
# head's own position was a person turning their head in a live stream, at the
# cost of a fresh pairing. Asserted here instead, with no guest, no headset and
# no Steam host. Seconds; in `make check`.
build/t_xrspace: tests/t_xrspace.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_xrspace.c $(RUNTIME) $(LDLIBS)

xrspace: build/t_xrspace
	./build/t_xrspace

# The OpenXR action-surface gate — the same argument as xrspace above,
# one API family across. A binding decoded to the wrong bit, two hands combined
# the wrong way, a stale press surviving a controller being put down, an action
# space following the other hand: every one of those returns XR_SUCCESS and
# draws a correct picture, and the only instrument that could see one is a
# person holding a controller inside a live stream. No guest, no headset, no
# Steam host; seconds, in `make check`.
build/t_xrinput: tests/t_xrinput.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_xrinput.c $(RUNTIME) $(LDLIBS)

xrinput: build/t_xrinput
	./build/t_xrinput

# The on-screen keyboard. A guest that wants text hands the job to the platform
# and waits; here a FRONTEND is the platform, and every way that seam can be
# wrong is silent — a report nothing drains, a close that arrives before the
# final string, a cancellation reported as a commit, a caret left behind the
# text. The only other instrument is a person clicking a field in a running
# guest, which costs a viewer session and cannot be repeated identically. No
# guest, no window, no headset; milliseconds, in `make check`.
build/t_softinput: tests/t_softinput.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_softinput.c $(RUNTIME) $(LDLIBS)

softinput: build/t_softinput
	./build/t_softinput

build/t_variadic: tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(LDLIBS)

vatest: build/t_variadic
	./build/t_variadic

# kl_view.c is the KL_VIEW=1 interactive viewer (SDL3); only m_boot links it,
# so only this rule carries the pkg-config flags. kl_view.c compiles to a stub
# without SDL3 headers, but the link flags below assume pkg-config finds sdl3.
# tests/t_mtl_provider.m is the host stand-in for Compositor Services:
# Objective-C because it has to *create* MTLTextures. Host-only and diagnostic —
# named here and never in RUNTIME_SHIP, which is why the shipping runtime stays
# plain C and takes an opaque texture pointer.
build/m_boot: mains/m_boot.c tests/t_mtl_provider.m $(RUNTIME) runtime/gfx/kl_view.c \
              runtime/gfx/kl_view_mtl.m $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc $(shell pkg-config --cflags sdl3) -o $@ \
	  mains/m_boot.c tests/t_mtl_provider.m $(RUNTIME) runtime/gfx/kl_view.c \
	  runtime/gfx/kl_view_mtl.m \
	  $(LDLIBS) -framework Metal -framework QuartzCore -framework Foundation \
	  $(shell pkg-config --libs sdl3)

boot: build/m_boot
	./build/m_boot $(LIBS)

# ---- guest payloads we build ourselves ----
#
# A guest we control, so the veneers can be checked for CORRECT ANSWERS rather than
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

build/t_guest: tests/t_guest.c guest/torture.c guest/torture.h $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_guest.c guest/torture.c $(RUNTIME) $(LDLIBS)

guest: build/t_guest build/guest_torture.so
	./build/t_guest build/guest_torture.so

# The x18 decoder, checked against objdump over every guest library.
# Deliberately not linked against the runtime: it reads ELF files, it does not
# load them, so a decoder bug cannot hide behind a working loader.
build/t_x18: tests/t_x18.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_x18.c runtime/kl_x18.c runtime/kl_env.c

x18: build/t_x18
	python3 tools/check_x18.py build/t_x18 $(foreach f,$(GUEST_LIBS),$(LIBS)/$(f).so)

# The CTR_EL0 veneer, EXECUTED. Same shape as t_x18: linked against
# the decoder alone, so a veneer bug cannot hide behind a working loader. It
# runs the real illegal instruction in a child as its control, which is why the
# host can gate a crash that only ever happened on a headset.
build/t_ctr: tests/t_ctr.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_ctr.c runtime/kl_x18.c runtime/kl_env.c

ctr: build/t_ctr
	./build/t_ctr

# The broadcast fan-out, driven through the shim's own sendto and aimed
# at loopback, so it exercises the discovery path without putting 254 datagrams
# on anyone's network. The path is otherwise reachable only from a live Qt
# frontend with a Steam host on the LAN.
build/t_bcast: tests/t_bcast.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_bcast.c $(RUNTIME) $(LDLIBS)

bcast: build/t_bcast
	./build/t_bcast

# ...and the preemptive version of the same question: what OTHER system
# registers do these guests touch, and which of them may EL0 not execute?
#
# ONE instruction in ONE library can be illegal from EL0 with nothing below the
# device taking that path. This decodes every
# `mrs`/`msr`/`sys` in every guest library, names the register, and sorts by
# whether Darwin permits it — so a new target's answer is a command rather than
# a crash. Reads ELF files; no guest runs, seconds.
.PHONY: sysregs
sysregs:
	python3 tools/sysreg_scan.py $(call guest_elf_libs,$(LIBS)) \
	  $(SLVRLIBS)/*.so $(SLLIBS)/*.so

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
	# The 2D shell's chain, which the device loads too — and which is
	# where the CTR_EL0 read lives (libQt6Core carries the project's only
	# `mrs CTR_EL0`). libshell is the data-in-.text case: 12 of its 14 apparent
	# x18 sites are CRYPTOGAMS constants with no symbol over them.
	python3 tools/check_x18.py build/t_x18 $(SLVRLIBS)/libshell_arm64-v8a.so \
	  $(SLVRLIBS)/libQt6Core_arm64-v8a.so $(SLVRLIBS)/libQt6Gui_arm64-v8a.so \
	  $(SLVRLIBS)/libQt6Widgets_arm64-v8a.so $(SLVRLIBS)/libQt6Network_arm64-v8a.so

# kl_jni_slots.h is checked in; regenerate only when bumping the NDK.
jnislots:
	python3 tools/gen_jni_slots.py > runtime/kl_jni_slots.h

# ...and kl_target_table.h likewise, from visionos/targets.py — regenerate after
# adding or editing a target there. Checked in for the same reason the other two
# are: a build must not depend on Python, and the diff of a generated table is
# worth reading.
.PHONY: targets
targets:
	python3 visionos/targets.py --c-table > runtime/kl_target_table.h
	@echo "runtime/kl_target_table.h: $$(python3 visionos/targets.py --list | tr '\n' ' ')"

# ...and kl_cacert_table.h, the root CA anchors the guest's TLS validates
# against. Checked in for a reason the other generated tables do not have:
# SecTrustCopyAnchorCertificates is __IPHONE_NA, so the visionOS build COULD not
# read the store even if it were willing to depend on Python. One table,
# generated on a Mac, compiled into host and device alike.
#
# Roots expire and get withdrawn, so this one goes stale on a clock rather than
# on a code change. Regenerate deliberately and read the diff.
.PHONY: cacerts
cacerts:
	python3 tools/gen_cacerts.py > runtime/kl_cacert_table.h
	@grep -E '^// (source|dated|anchors):' runtime/kl_cacert_table.h

# The substitute default HRTF: CIPIC subject 124, byte-identical to
# core/data/hrtf/cipic_124.sofa in ValveSoftware/steam-audio (Apache-2.0).
# kl_phonon_hrtf.S .incbins it and kl_dl.c hands it to iplHRTFCreate when the
# guest asks for type=DEFAULT — VRChat's libphonon ships only a 40-byte stub
# there. Refetch deliberately; the shasum is the pin.
.PHONY: phonon-hrtf
phonon-hrtf:
	curl -L -o runtime/data/phonon_hrtf_cipic_124.sofa \
	    https://raw.githubusercontent.com/ValveSoftware/steam-audio/master/core/data/hrtf/cipic_124.sofa
	echo "c28ff4a874ac889ec0c5885ca524762a70d56984232ff7aadcd9c15d32e1cfb6  runtime/data/phonon_hrtf_cipic_124.sofa" | shasum -a 256 -c

build/t_il2cpp: tests/t_il2cpp.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_il2cpp.c $(RUNTIME) $(LDLIBS)

il2cpp: build/t_il2cpp
	./build/t_il2cpp

# Full regression sweep — run this first when picking work back up.
#
# Each test writes to a log and is checked BEFORE the log is filtered. Piping a
# test straight into tail/grep would hand make the filter's exit status instead
# of the test's, so a failing test would leave the sweep green.
check: build/t_opus build/t_variadic build/t_load build/t_il2cpp build/m_boot build/t_haptics build/t_hevc build/t_xrspace build/t_xrinput build/t_softinput build/t_ctr build/t_bcast
	@echo "=== variadic ABI ===" && ./build/t_variadic
	@./build/t_hevc > build/hevc.log 2>&1 || { cat build/hevc.log; exit 1; }
	@grep -E '=== HEVC|30 access units' build/hevc.log && tail -1 build/hevc.log
	@./build/t_xrspace > build/xrspace.log 2>&1 || { cat build/xrspace.log; exit 1; }
	@head -2 build/xrspace.log && tail -1 build/xrspace.log
	@./build/t_xrinput > build/xrinput.log 2>&1 || { cat build/xrinput.log; exit 1; }
	@grep -E '=== OpenXR actions' build/xrinput.log && tail -1 build/xrinput.log
	@./build/t_softinput > build/softinput.log 2>&1 || { cat build/softinput.log; exit 1; }
	@head -1 build/softinput.log && tail -1 build/softinput.log
	@./build/t_ctr > build/ctr.log 2>&1 || { cat build/ctr.log; exit 1; }
	@head -2 build/ctr.log && tail -1 build/ctr.log
	@./build/t_bcast > build/bcast.log 2>&1 || { cat build/bcast.log; exit 1; }
	@head -1 build/bcast.log && tail -1 build/bcast.log
	@./build/t_haptics > build/haptics.log 2>&1 || { cat build/haptics.log; exit 1; }
	@head -3 build/haptics.log && tail -1 build/haptics.log
	@$(MAKE) -s ovrpabi
# The opus roundtrip is the one gate here that RUNS guest code rather than
# inspecting it, so it is worth keeping wherever it exists — but libunityopus.so
# is a Unity 2019.x artifact (2018.4 has opus inside libunity and exports none of
# its API), so on an older title there is nothing to point it at. Skip loudly
# rather than fail: a missing library is a property of the guest, not a
# regression, and a silent skip is how a gate quietly stops covering anything.
	@echo "=== opus roundtrip ===" && if [ -f $(LIBS)/libunityopus.so ]; then \
	  ./build/t_opus $(LIBS)/libunityopus.so > build/opus.log && tail -3 build/opus.log; \
	else echo "  SKIPPED: $(LIBS) has no libunityopus.so (pre-2019 Unity keeps"; \
	     echo "  opus inside libunity). Guest code still executes under 'make boot'."; fi
	@echo "=== all guest libraries ===" && for f in $(GUEST_LIBS); do \
	  printf '%-24s' $$f; ./build/t_load $(LIBS)/$$f.so 2>/dev/null | grep -E '^  imports:'; done
	@echo "=== x18 veneers ===" && for f in $(GUEST_LIBS); do \
	  printf '%-24s' $$f; ./build/t_load $(LIBS)/$$f.so 2>/dev/null | grep -E '^  x18 sites:'; done
	@echo "  (refused sites are br x18 jump tables and data words in .text."
	@echo "   The count is guest-specific, so it moves with the APK; what must not"
	@echo "   move is the count for a GIVEN guest. 'make x18' is the exhaustive decoder"
	@echo "   check against objdump, run it after any change to runtime/kl_x18.c)"
# The TLS rewrite counts, watched for the same reason the veneer totals are: a
# single site the data test refuses leaves libunity reading a register the
# kernel clobbers, and `make check` stops at initJni, so it stays green while
# the lifecycle faults on it. A REFUSED TLS site is a hard failure here, not a
# statistic — unlike the x18 side, refusing one is never free.
	@echo "=== TLS rewrites ===" && for f in $(GUEST_LIBS); do \
	  printf '%-24s' $$f; ./build/t_load $(LIBS)/$$f.so 2>/dev/null > build/tls-$$f.log; \
	  grep -E '^  TLS rewrites:' build/tls-$$f.log; \
	  if grep -q 'TLS sites REFUSED' build/tls-$$f.log; then \
	    echo "  FAIL: $$f has a TLS site the data test refused — that thread pointer"; \
	    echo "        is garbage on every thread. The loader named the address."; \
	    exit 1; fi; \
	done
	@if [ -x "$(NDK_CC)" ]; then echo "=== guest differential ===" && \
	   $(MAKE) -s guest | grep -E 'x18 sites|identical to the host|lost '; \
	 else echo "=== guest differential: SKIPPED (set ANDROID_NDK_HOME) ==="; fi
	@echo "=== il2cpp runtime ===" && ./build/t_il2cpp $(LIBS)/libil2cpp.so $(TARGET)/assets/bin/Data/Managed > build/il2cpp.log && tail -4 build/il2cpp.log
	@echo "=== guest entry (JNI_OnLoad, initJni) ===" && ./build/m_boot $(LIBS) > build/boot.log 2>/dev/null && \
	  grep -E 'guard verified|DLC enumeration|JNI_OnLoad returned|registered com|load returned|natives registered:|ids requested:|EXIT CRITERION' build/boot.log

# ---- the translated-dylib path / the visionOS port ----
#
# klepton-ld translates a guest ELF .so into a Mach-O dylib so dyld — not our
# mmap loader — maps the guest text. It links runtime/kl_x18.c because that
# decoder is the authority on what an x18 site is; a bit-field guess reports
# 2158 sites in libunityopus.so where the decoder correctly reports 0.
# kl_env.c is here because kl_x18.c reads knobs through it. It had been missing
# for as long as kl_env existed and nothing noticed, because the rule's only
# prerequisites were sources it already had — so an EXISTING build/klepton-ld
# was never remade, and the link error only appeared the first time something
# forced a rebuild. A stale translator is not a harmless one: it bakes
# KLX_TSD_SLOT into every veneer it emits.
build/klepton-ld: tools/klepton_ld.c runtime/kl_x18.c runtime/kl_env.c runtime/kl_x18.h \
                  runtime/guest/kl_guestpatch.c runtime/guest/kl_guestpatch.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/klepton_ld.c runtime/kl_x18.c runtime/kl_env.c \
	    runtime/guest/kl_guestpatch.c

# P1 — the same t_opus roundtrip the mmap loader passes, through the translated dylib.
# t_opus picks its loader from the file's magic, so this is one test over two
# loaders rather than two tests. arm64 macOS requires at least an ad-hoc
# signature before dyld will map anything.
.PHONY: p1
p1: build/klepton-ld build/t_opus
	@test -f $(LIBS)/libunityopus.so || { \
	  echo "P1 SKIPPED: $(LIBS) has no libunityopus.so — this title keeps opus"; \
	  echo "  inside libunity and exports none of its API, so there is no pure"; \
	  echo "  guest function to round-trip. 'make bootdylib-life' is the"; \
	  echo "  equivalent proof for such a title: the whole guest runs from dylibs."; \
	  exit 0; }
	@./build/klepton-ld $(LIBS)/libunityopus.so -o build/libunityopus.dylib
	@codesign -s - -f build/libunityopus.dylib 2>/dev/null
	@./build/t_opus $(PWD)/build/libunityopus.dylib | tail -4

# P2 — the same dylib under the visionOS Simulator's dyld, which is where a
# malformed hand-emitted Mach-O gets caught (rung 2 of the ladder). Needs a booted
# visionOS simulator; XRSIM overrides which one.
XRSIM ?= $(shell xcrun simctl list devices booted 2>/dev/null | grep -o '[0-9A-F-]\{36\}' | head -1)
.PHONY: p2
p2: build/klepton-ld
	@test -n "$(XRSIM)" || { echo "no booted simulator — 'xcrun simctl boot <udid>' first"; exit 1; }
	@./build/klepton-ld $(LIBS)/$(DYLIB_PROBE_LIB).so -o build/$(DYLIB_PROBE_LIB)-xrsim.dylib \
	   --platform visionossim --quiet
	@codesign -s - -f build/$(DYLIB_PROBE_LIB)-xrsim.dylib 2>/dev/null
	@clang -target arm64-apple-xros1.0-simulator \
	   -isysroot $$(xcrun --sdk xrsimulator --show-sdk-path) -arch arm64 \
	   -o build/dl_xrsim tools/dlprobe.c
	@xcrun simctl spawn $(XRSIM) $(PWD)/build/dl_xrsim $(PWD)/build/$(DYLIB_PROBE_LIB)-xrsim.dylib

# Translate every guest library. All five go through now that the x18 veneer
# pass runs offline; a library klepton-ld cannot take is reported rather than
# skipped silently, so a regression here reads as one.
.PHONY: dylibs
dylibs: build/klepton-ld
	@mkdir -p build/dylibs
	@for f in $(GUEST_LIBS); do \
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
	  grep -aE 'loaded as a translated dylib|guard verified|natives registered:|ids requested:|EXIT CRITERION' \
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
	@for f in $(GUEST_LIBS); do \
	  test -f build/dylibs/$$f.dylib || continue; \
	  printf '  %-22s' $$f; \
	  KL_DYLIB_DIR=$(PWD)/build/dylibs ./build/t_load $(LIBS)/$$f.so 2>&1 \
	    | grep -E '^  imports:' | tr -d '\n'; echo; done

# ---- the runtime builds for visionOS ----
#
# Two gates, and the second is the one that carries the information. Compiling
# proves the headers are there; *linking* proves every symbol the runtime
# references is present in the SDK, which is where an API that is declared but
# absent — or present on macOS and nowhere else — actually shows up.
# -all_load forces every object in and -undefined error refuses to defer, so a
# missing symbol inside a function nothing calls yet still fails the build.
#
# Both platforms are built BY DEFAULT, because they diverge: xrsimulator is a
# macOS-kernel host with an iOS-shaped SDK, so a header present only there would
# pass the simulator and fail the device. That is the GATE's reason and it stays
# the default.
#
# `XROS_PLATFORMS=xros` (or `=xrsim`) builds one, which is what a run of the app
# actually needs — a device build never loads the simulator slice — and it is
# half the work: 14.5 s of a 30 s incremental `make xros` on this hardware.
# `visionos/run.sh` asks for the one its mode will install. Building one is NOT
# the gate and the line at the end of the rule says so rather than claiming
# both.
XROS_PLATFORMS ?= xros xrsim
$(foreach p,$(XROS_PLATFORMS),$(if $(filter xros xrsim,$(p)),,\
  $(error XROS_PLATFORMS: '$(p)' is not one of xros xrsim)))
XROS_SDK  := $(shell xcrun --sdk xros --show-sdk-path)
XRSIM_SDK := $(shell xcrun --sdk xrsimulator --show-sdk-path)
# The gate name and the archive for each platform, so nothing below repeats the
# mapping.
XROS_GATE_xros  := xros-device
XROS_GATE_xrsim := xros-sim
XROS_GATES := $(foreach p,$(XROS_PLATFORMS),$(XROS_GATE_$(p)))
XROS_LIBS  := $(foreach p,$(XROS_PLATFORMS),build/$(p)/libklepton.a)
# $(MVK_INC) for the same reason CFLAGS carries it, and its absence here was a
# real hole rather than an omission: kl_vulkan.c is guarded by __has_include, so
# without the Vulkan headers on the command line it still COMPILES — as the stub
# that refuses by name. The device build therefore had no Vulkan at all while the
# host had a working one, which is the worst shape a divergence can take (nothing
# fails; a Vulkan guest simply reports that the hardware does not meet its
# requirements, on device only). Nothing links against MoltenVK either way —
# kl_vulkan.c is VK_NO_PROTOTYPES and dlopens it — so this is headers only.
XROS_CFLAGS := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 $(MVK_INC) $(RUNTIME_INC)

.PHONY: xros xros-device xros-sim swiftcheck
xros: $(XROS_GATES) build/Klepton.xcframework swiftcheck
	@if [ "$(words $(XROS_PLATFORMS))" = 2 ]; then \
	  echo "  gate: the shipping runtime compiles and links for both visionOS platforms."; \
	else \
	  echo "  XROS_PLATFORMS=$(XROS_PLATFORMS) — built for that platform ONLY; the"; \
	  echo "  gate is the pair, and the other half is unproven by this run."; \
	fi

# ...and the OTHER half of that seam, which had no gate at all: the Swift app
# against the C headers it imports through the bridging header.
#
# `xros` proves the runtime compiles and links. It says nothing about whether
# the app still calls it correctly — and every C signature the app touches is a
# signature Swift binds by shape, so widening one (the eye-texture provider
# gaining its GL internalformat) breaks KleptonCompositor.swift with
# nothing on the host reporting it. The next time anyone found out was a
# `visionos/run.sh` that needs a booted simulator or a headset.
#
# -typecheck only: no objects, no linking, a few seconds, and it needs neither
# a simulator nor a device. The deployment target has to match
# gen_xcodeproj.py's XROS_DEPLOYMENT_TARGET or availability answers differ
# between this and the real build, which is the one thing that would make it
# lie.
swiftcheck:
	@xcrun -sdk xros swiftc -typecheck -target arm64-apple-xros26.0 \
	  -import-objc-header visionos/Sources/Klepton-Bridging-Header.h \
	  $(RUNTIME_INC) visionos/Sources/*.swift 2>&1 | grep -E '^[^ ].*error:' && exit 1; \
	  echo "  swiftcheck: the visionOS app typechecks against runtime/*.h"

# The archives are real targets with real prerequisites, not side effects of a
# .PHONY rule. That distinction cost a debugging pass: with only a phony rule
# producing it, an edited kl_image.c left a stale libklepton.a in place, the
# xcframework was built from it, and the app in the simulator silently ran the
# *previous* loader — which reads as a bug in the app rather than in the build.
# The headers are prerequisites too, and that is not pedantry: KLX_TSD_SLOT and
# every other cross-file constant lives in one, so without them a header-only
# change leaves a stale archive, the xcframework is built from it, and the app
# runs the PREVIOUS constant while the source says otherwise. That is the same
# stale-artifact trap the xcframework rule below already records; the fix there
# stopped at the archives and left this behind it.
build/xros/libklepton.a: $(RUNTIME_SHIP) $(RUNTIME_ALL_HDRS)
	@mkdir -p build/xros
	@rm -f $@
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xros/$$(basename $$s | sed 's/\.[cSm]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) \
	    -c $$s -o $$o || exit 1; done
	@ar rcs $@ build/xros/*.o

build/xrsim/libklepton.a: $(RUNTIME_SHIP) $(RUNTIME_ALL_HDRS)
	@mkdir -p build/xrsim
	@rm -f $@
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xrsim/$$(basename $$s | sed 's/\.[cSm]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) \
	    -c $$s -o $$o || exit 1; done
	@ar rcs $@ build/xrsim/*.o

# The gate: link each archive into a dylib with -all_load, so a symbol that is
# declared in a header but absent from the platform fails the build even inside
# a function nothing calls yet.
build/xros/libklepton.dylib: build/xros/libklepton.a
	@$(CC) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -framework AudioToolbox \
	   -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
	   -framework CoreFoundation -framework AVFoundation -framework Foundation \
	   -install_name @rpath/libklepton.dylib

build/xrsim/libklepton.dylib: build/xrsim/libklepton.a
	@$(CC) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -framework AudioToolbox \
	   -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
	   -framework CoreFoundation -framework AVFoundation -framework Foundation \
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
# ...and WHICH SLICES it holds is a prerequisite too, for the same reason and
# with a sharper edge: with XROS_PLATFORMS selectable, an xcframework built for
# the device is up to date by every timestamp rule when the next build wants the
# simulator, and Xcode then fails on an arch mismatch that names neither this
# file nor the variable. The stamp holds the slice list, so changing it rebuilds
# and nothing else does.
build/.xcframework-slices: FORCE
	@mkdir -p build
	@printf '%s\n' '$(XROS_PLATFORMS)' | cmp -s - $@ 2>/dev/null || \
	  printf '%s\n' '$(XROS_PLATFORMS)' > $@
.PHONY: FORCE
FORCE:

build/Klepton.xcframework: $(XROS_LIBS) $(RUNTIME_ALL_HDRS) build/.xcframework-slices
	@rm -rf $@
	@xcodebuild -create-xcframework \
	   $(foreach l,$(XROS_LIBS),-library $(l) -headers runtime) \
	   -output $@ > /dev/null
	@echo "  build/Klepton.xcframework ($(XROS_PLATFORMS))"

# ANGLE for visionOS. The vendored checkout's gn has no xros target and does not
# need one: build for iOS — which already enables the Metal backend — and rewrite
# LC_BUILD_VERSION afterwards. Confirmed prior art (ALVR ships a Rust dylib this
# way). ANGLE emits ios_framework_bundle on iOS, so the output is already the
# .framework packaging the app bundle wants.
.PHONY: angle-ios angle-ios-sim angle-xros
angle-ios: angle-fetch
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gn gen out/ios --args='is_debug=false target_os="ios" target_cpu="arm64" \
	    target_environment="device" ios_enable_code_signing=false \
	    angle_enable_vulkan=false angle_enable_swiftshader=false' && \
	  autoninja -C out/ios libEGL libGLESv2

# The simulator slice. Same trick one platform over: an iOS *simulator* build
# vtool'd to visionossim. Needed because the simulator is the fast development
# loop — a device round trip is minutes, the simulator is seconds — and
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
# s07/s08 probe Apple's desktop GL; s09 probes ANGLE. None of them link the
# runtime — they answer questions about the host, not about the shim.
build/s07_glfb: spikes/s07_glfb.c
	$(CC) $(CFLAGS) -o $@ $< -framework OpenGL -lz
build/s08_glsl: spikes/s08_glsl.c
	$(CC) $(CFLAGS) -o $@ $< -framework OpenGL
build/s09_angle: spikes/s09_angle.c
	$(CC) $(CFLAGS) -o $@ $<
build/s10_shared: spikes/s10_shared.c
	$(CC) $(CFLAGS) -o $@ $<

# The compositor interop primitive: ANGLE rendering into an MTLTexture we own.
# Objective-C because the spike has to *create* the MTLTexture; the shipping
# path never does — it receives one from Swift as an opaque pointer, which is
# why kl_glfb stays plain C.
build/s11_mtltex: spikes/s11_mtltex.m
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< -framework Metal -framework Foundation

.PHONY: mtltex
mtltex: build/s11_mtltex
	@./build/s11_mtltex

# Foveation: does ANGLE rasterize through an MTLRasterizationRateMap we
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
build/t_reproject: tests/t_reproject.m runtime/gfx/kl_reproject.c runtime/gfx/kl_reproject.h \
                   runtime/kl_env.c runtime/xr/kl_ovrp.h
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< runtime/gfx/kl_reproject.c \
	  runtime/kl_env.c -framework Metal -framework Foundation

.PHONY: reproject
reproject: build/t_reproject
	@./build/t_reproject

# The haptics seam's model, with no guest and no headset: the three
# OVRPlugin entry points AS THE GUEST RESOLVES THEM (through kl_ovrp_sym, so
# the struct returns go through the real sret thunk), the queue behind them,
# and the pulses a frontend pulls. Fast and deterministic; part of `check`.
build/t_haptics: tests/t_haptics.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_haptics.c $(RUNTIME) $(LDLIBS)

.PHONY: haptics
haptics: build/t_haptics
	@./build/t_haptics

# ...and the half t_haptics cannot see: whether our implementations agree with
# the REAL libOVRPlugin.so about which arguments are OUT-parameters. Offline —
# it disassembles the plugin in the guest tree, runs no guest and needs no
# hardware, and takes about a second. It exists because that question has cost
# two arcs now: the return-value half (ovrpResult vs ovrpBool), and the argument
# half — an out pointer we never wrote, which is a strlen(NULL) in
# whichever native library reads it and names nothing on the way down.
# LIBS points it at whichever guest is current; a tree with no libOVRPlugin.so
# says so and passes.
.PHONY: ovrpabi
ovrpabi:
	@python3 tools/ovrp_abi.py $(LIBS)/libOVRPlugin.so runtime/xr/kl_ovrp.c

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

# ---- MoltenVK (BONELAB / the synthetic libvulkan.so arc) ----------------------
#
# BONELAB boots completely and cannot render because the build's graphics API is
# Vulkan. MoltenVK is the host side of the answer, and unlike
# ANGLE it is vendored as a PREBUILT: nothing here patches it, so a 180 MB
# tarball buys what an ~18 GiB checkout and a 30-60 minute build would.
#
# Two things about the retarget are worth knowing before editing either script,
# because both invert what the ANGLE block next door does:
#
#   * the PLATFORM needs no forgery. Khronos ships native `xros-arm64` and
#     `xros-arm64_x86_64-simulator` slices in MoltenVK-all.tar, already stamped
#     VISIONOS / VISIONOSSIMULATOR. angle_retarget.sh's iOS->visionOS trick is
#     not needed and would be strictly worse than a real build.
#   * the FLOOR does. The release is built against the visionOS 26.5 SDK and
#     stamps `minos 26.5`; dyld refuses an image whose minimum exceeds the OS,
#     and this machine's SDK is 26.0. That is what vtool fixes here.
#
# vendor-moltenvk/ is gitignored exactly as vendor/ is — the tracked artifacts
# are the two scripts and the version+sha256 pin inside mvk_fetch.sh.
.PHONY: mvk mvk-fetch mvk-retarget mvk-check mvk-status
mvk-fetch:
	@./tools/mvk_fetch.sh

mvk-retarget:
	@./tools/mvk_retarget.sh

# The build-it-once target: pull and stage all three slices. Both halves are
# idempotent, so re-running is a fast no-op.
mvk: mvk-fetch
	@./tools/mvk_retarget.sh

MVK_OUT := vendor-moltenvk/out

# The host half of the gate — MoltenVK loaded and RUN, not inspected. See
# tests/t_mvk.c for why every cheaper check is a false answer.
build/t_mvk: tests/t_mvk.c
	@mkdir -p build
	@test -d $(MVK_OUT)/include || { echo "  !! run 'make mvk' first"; exit 1; }
	$(CC) $(CFLAGS) -I$(MVK_OUT)/include -o $@ tests/t_mvk.c

# ...and the visionOS half, which the host cannot run: link against the
# retargeted device slice with the LOCAL SDK. This is what proves the lowered
# floor is consumable rather than merely written — a framework whose minos still
# exceeded the deployment target fails right here, at the link, instead of on a
# headset. `-Wl,-no_weak_imports` is deliberate: it turns "this symbol will be
# missing at runtime on your floor" from a silent weak import into a link error,
# which is the whole question being asked about a 26.5 binary stamped 1.0.
mvk-check: build/t_mvk
	@echo "=== MoltenVK: visionOS device slice, linked against the local SDK"
	@mkdir -p build
	@printf '#include <vulkan/vulkan.h>\nVkResult klmvk_probe(const VkInstanceCreateInfo *ci, VkInstance *out);\nVkResult klmvk_probe(const VkInstanceCreateInfo *ci, VkInstance *out) { return vkCreateInstance(ci, 0, out); }\n' > build/mvk_link.c
	@$(CC) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) -arch arm64 \
	   -I$(MVK_OUT)/include -dynamiclib -o build/mvk_link.dylib build/mvk_link.c \
	   -F$(MVK_OUT)/xros -framework MoltenVK -Wl,-no_weak_imports \
	   -install_name @rpath/mvk_link.dylib
	@printf '  ok   links for xros1.0 against %s\n' "$(MVK_OUT)/xros/MoltenVK.framework"
	@echo
	@./build/t_mvk

mvk-status:
	@if [ ! -d $(MVK_OUT) ]; then echo "  vendor-moltenvk/ absent — run 'make mvk'"; else \
	  for d in xros xrsim; do \
	    b=$(MVK_OUT)/$$d/MoltenVK.framework/MoltenVK; \
	    if [ -f $$b ]; then printf '  %-8s ' $$d; \
	      xcrun vtool -show-build $$b | awk '/platform/{p=$$2} /minos/{m=$$2} /sdk/{s=$$2} \
	        END{printf "%-20s minos %-6s sdk %s\n", p, m, s}'; \
	    else printf '  %-8s absent\n' $$d; fi; done; \
	  b=$(MVK_OUT)/macos/libMoltenVK.dylib; \
	  if [ -f $$b ]; then printf '  %-8s ' macos; \
	    xcrun vtool -show-build -arch arm64 $$b | awk '/platform/{p=$$2} /minos/{m=$$2} \
	      END{printf "%-20s minos %s\n", p, m}'; \
	  else printf '  %-8s absent\n' macos; fi; \
	fi

# `make angle` / `make shared` exercise the vendored debug build — build it
# with `make angle-debug` first.
.PHONY: angle
angle: build/s09_angle
	@./build/s09_angle

# Shared ANGLE contexts across threads. Runs the matrix that separates
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

# The GL tracing trampoline's ABI contract (runtime/gfx/kl_gl_trace.S). It forwards an
# unknown signature, so nothing else can check it — and a trampoline that drops a
# register produces a wrong picture rather than a crash.
build/t_trace: tests/t_trace.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_trace.c $(RUNTIME) $(LDLIBS)

.PHONY: trace
trace: build/t_trace
	@./build/t_trace
