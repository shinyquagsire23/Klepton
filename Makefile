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
# RUNTIME_SHIP — everything that links the runtime needs them. Metal likewise,
# for kl_mtl_foveate.m, which makes descriptors of its own.
CFLAGS += $(REPRO_FLAGS)
LDLIBS  := -lz -framework AudioToolbox -framework CoreAudio \
           -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
           -framework CoreFoundation -framework AVFoundation -framework Foundation \
           -framework Metal
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
           runtime/jni/kl_jni_ue4.c runtime/jni/kl_jni_electra.c runtime/jni/kl_jni_fmod.c runtime/jni/kl_jni_jkxr.c \
           runtime/jni/kl_jni_haptics.c \
           runtime/jni/kl_jni_firebase.c runtime/jni/kl_jni_vivox.c \
           runtime/jni/kl_jni_vertigo.c runtime/jni/kl_jni_ghosttown.c

# The Linux process runtime (runtime/linux/) and its door. Part of what ships:
# a Linux guest in the visionOS app is the same code as build/m_linux.
LX_SRC  := runtime/linux/kl_lx_exec.c runtime/linux/kl_lx_syscall.c runtime/linux/kl_lx_svc.S \
           runtime/linux/kl_lx_vfs.c runtime/linux/kl_lx_mm.c runtime/linux/kl_lx_thread.c \
           runtime/linux/kl_lx_proc.c runtime/linux/kl_lx_signal.c runtime/linux/kl_lx_event.c \
           runtime/linux/kl_lx_hostlib.c runtime/linux/kl_lx_vulkan.c runtime/linux/kl_lx_steam.c \
           runtime/linux/kl_lx_steam_net.c runtime/linux/kl_lx_openxr.c \
           runtime/linux/kl_lx_diag.c runtime/linux/kl_lx_audio.c \
           runtime/linux/kl_lx_interstitial.c

RUNTIME_SHIP := $(LX_SRC) runtime/guest/kl_linuxproc.c \
           runtime/kl_env.c runtime/kl_image.c runtime/kl_stub_cells.S runtime/libc/kl_shim.c runtime/libc/kl_va.c \
           runtime/libc/kl_va_handlers.c runtime/libc/kl_va_thunks.S \
           runtime/libc/kl_libc.c runtime/libc/kl_libc_slink.c runtime/libc/kl_pthread.c runtime/kl_dl.c \
           runtime/guest/kl_ndk.c runtime/kl_x18.c runtime/kl_target.c \
           $(RUNTIME_JNI) \
           runtime/gfx/kl_egl.c runtime/media/kl_opensl.c runtime/media/kl_audio.c runtime/xr/kl_ovrp.c runtime/xr/kl_hands.c \
           runtime/xr/kl_ovrp_sret.S runtime/gfx/kl_reproject.c runtime/gfx/kl_present.c \
           runtime/xr/kl_ovrplat.c runtime/xr/kl_assetcfg.c runtime/xr/kl_room.c runtime/xr/kl_steamapi.c runtime/xr/kl_openxr.c runtime/media/kl_mediandk.c runtime/media/kl_vtdec.c runtime/media/kl_avdec.m \
           runtime/gfx/kl_vulkan.c runtime/gfx/kl_vk_eyefix.c runtime/gfx/kl_mtl_foveate.m runtime/gfx/kl_mtl_busy.m \
           runtime/guest/kl_nativeactivity.c runtime/guest/kl_slink.c runtime/guest/kl_ue4.c runtime/guest/kl_obbmap.c \
           runtime/guest/kl_jkxr.c runtime/guest/kl_native.c runtime/guest/kl_xashservers.c runtime/guest/kl_sdl2.c \
           runtime/guest/kl_gles3jni.c runtime/guest/kl_driver.c \
           runtime/media/kl_aaudio.c runtime/media/kl_mediaplayer.c \
           runtime/gfx/kl_glfb.c runtime/gfx/kl_cvmtl.m runtime/gfx/kl_fove.m runtime/gfx/kl_gl_trace.S runtime/gfx/kl_gl_lock.S \
           runtime/guest/kl_mono.c \
           runtime/guest/kl_il2cpp.c runtime/kl_fault.c runtime/guest/kl_guestpatch.c \
           runtime/guest/kl_guestpoke.c runtime/kl_cacerts.c runtime/media/kl_phonon_hrtf.S \
           runtime/media/kl_menu_font.S \
           runtime/diag/kl_sample.c runtime/diag/kl_mprobe.c runtime/diag/kl_stall.c runtime/diag/kl_metrics.c \
           runtime/guest/kl_extension.c runtime/guest/kl_aot_hooks.c \
           runtime/guest/kl_songs.c
RUNTIME_DIAG := runtime/diag/kl_metadump.c
RUNTIME := $(RUNTIME_SHIP) $(RUNTIME_DIAG)

# Scotland2 is an optional leaf component. Host-specific tests opt in by target.
KLEPTON_SCOTLAND2 ?= $(if $(filter build/t_scotland2%,$(MAKECMDGOALS)),1,0)
ifeq ($(KLEPTON_SCOTLAND2),1)
SC2_FRAMEWORKS := -framework CoreText -framework CoreGraphics
RUNTIME_INC += -Iports/scotland2/include
SC2_DIAGNOSTICS := $(if $(filter release,$(SC2_BUILD_MODE)),0,1)
SC2_DEFINES := -DKL_GUEST_EXTENSION_BUILTIN -DSC2_DIAGNOSTICS=$(SC2_DIAGNOSTICS)
CFLAGS += -Iports/scotland2/include $(SC2_DEFINES)
LDLIBS += $(SC2_FRAMEWORKS)
RUNTIME_SHIP += ports/scotland2/src/host-bridge.c ports/scotland2/src/apple-unicode.m
RUNTIME := $(RUNTIME_SHIP) $(RUNTIME_DIAG)
endif

# A source-list switch must rebuild archives and host fixtures in both directions.
.PHONY: runtime-selection-force
build/runtime-selection: runtime-selection-force
	@mkdir -p build
	@printf '%s\n' 'scotland2=$(KLEPTON_SCOTLAND2) diagnostics=$(SC2_DIAGNOSTICS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

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
RUNTIME_ALL_HDRS := $(wildcard runtime/*.h) $(wildcard runtime/*/*.h) $(wildcard ports/scotland2/include/*.h) build/runtime-selection Makefile
RUNTIME_HDRS := $(RUNTIME_ALL_HDRS) $(wildcard tests/*.h) \
                runtime/data/phonon_hrtf_cipic_124.sofa \
                runtime/data/kl_menu_font.ttf

.PHONY: all test clean check load vatest il2cpp boot jnislots x18 guest x18race uevk jniobj
all: build/t_opus

# Apple Scotland2 core: explicit host gate, separate from the stock runtime.
SC2_CORE_SRC := $(wildcard ports/scotland2/src/*) $(wildcard ports/scotland2/include/*) \
                $(wildcard ports/scotland2/upstream/src/*) \
                $(wildcard ports/scotland2/upstream/shared/*) \
                $(wildcard ports/scotland2/upstream/include/*) \
                $(wildcard port-deps/sources/fmt/include/fmt/*) ports/scotland2/build.sh
build/scotland2/macosx/libscotland2-core.a: $(SC2_CORE_SRC)
	bash ports/scotland2/build.sh macosx

build/t_scotland2: ports/scotland2/tests/klepton.c build/scotland2/macosx/libscotland2-core.a $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Iports/scotland2/include -o $@ ports/scotland2/tests/klepton.c \
	  $(RUNTIME) build/scotland2/macosx/libscotland2-core.a $(LDLIBS) -lc++

build/t_scotland2_il2cpp: ports/scotland2/tests/il2cpp.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ ports/scotland2/tests/il2cpp.c $(RUNTIME) $(LDLIBS)

build/t_android_1453: tests/t_android_1453.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_android_1453.c $(RUNTIME) $(LDLIBS)

build/t_scotland2_paper: ports/scotland2/tests/paper.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ ports/scotland2/tests/paper.c $(RUNTIME) $(LDLIBS)

build/t_scotland2_aot: ports/scotland2/tests/aot-hooks.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ ports/scotland2/tests/aot-hooks.c $(RUNTIME) $(LDLIBS)

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

# android_dlopen_ext through the guest-facing symbol table: the plain form is
# dlopen, and every extension flag is refused with a dlerror that names it. No
# guest library is loaded; milliseconds, in `make check`.
build/t_futex: tests/t_futex.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_futex.c $(RUNTIME) $(LDLIBS)

build/t_dl_ext: tests/t_dl_ext.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_dl_ext.c $(RUNTIME) $(LDLIBS)

.PHONY: dl-ext
dl-ext: build/t_dl_ext
	./build/t_dl_ext

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

# The Oculus Platform completion queue (runtime/xr/kl_ovrplat.c): more requests
# outstanding than a pool block, held handles across new requests, FreeMessage
# misuse, and the queue and the call-surface table under threads. Before it, a
# 17th outstanding request got an id and no completion, and two threads
# resolving names could tear a slot. No guest; seconds; in `make check`.
build/t_ovrplat: tests/t_ovrplat.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_ovrplat.c $(RUNTIME) $(LDLIBS)

.PHONY: ovrplat
ovrplat: build/t_ovrplat
	./build/t_ovrplat

# The pose-boundary gate for one pose per guest frame (from rebelancap's fork)
# — one head pose per guest frame cycle, and the anchor filed for reprojection
# equal to it. Unity's render and sim threads number the same frame differently
# in gameplay, so a boundary keyed on that number froze two head poses inside
# one frame (his `poses/frame` 1.70 instead of 1.00). A process per case,
# because the boundary is read once: the default; the index boundary, which
# must be SEEN to split; and the two kinds of guest that never call
# ovrp_WaitToBeginFrame (OpenXR frames, with and without an ovrp_Update2 in
# mid-frame), where one-pose must do nothing. Milliseconds; in `make check`.
build/t_posebound: tests/t_posebound.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_posebound.c $(RUNTIME) $(LDLIBS)

posebound: build/t_posebound
	./build/t_posebound

# Enqueued panels under KL_OVRP_ENQ_RETIRE (Ghost Town): a destroyed or expired
# panel is taken down and stays down. Milliseconds; in `make check`.
build/t_ovrplayers: tests/t_ovrplayers.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_ovrplayers.c $(RUNTIME) $(LDLIBS)

ovrplayers: build/t_ovrplayers
	./build/t_ovrplayers

# Tracker nodes 5..8 as a Quest answers them (KL_OVRP_NO_TRACKERS, Angry Birds VR). In `make check`.
build/t_trackers: tests/t_trackers.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_trackers.c $(RUNTIME) $(LDLIBS)

trackers: build/t_trackers
	./build/t_trackers

# The profile for untested games (kl_untested_profile): which titles, the
# OVRPlugin version read from a game's own library, and unknown Java methods
# answered with zero. Milliseconds; in `make check`.
build/t_untested: tests/t_untested.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_untested.c $(RUNTIME) $(LDLIBS)

untested: build/t_untested
	./build/t_untested

# The audio sink's gates (rebelancap's tests/t_audio.c and tests/t_aaudio.c,
# taken over with his sink). The first: a guest's sample format, per encoding,
# and the OpenSL surface driven with PCM and PCM_EX sources — a float32 guest
# played as int16 is half-speed static in one ear, and the [sl] line it
# produced read the same as a correct player's. Milliseconds; in `make check`.
build/t_audio: tests/t_audio.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_audio.c $(RUNTIME) $(LDLIBS)

.PHONY: audio-formats
audio-formats: build/t_audio
	@./build/t_audio

# The second: the block-seam harness. It models a guest's mixer — a copy
# callback in front of an asynchronous producer — so the AAudio path can be
# measured on the host. Real time (20 s by default) and it opens the real
# output device, at +-1 LSB, so it is NOT in `check`.
build/t_aaudio: tests/t_aaudio.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_aaudio.c $(RUNTIME) $(LDLIBS)

.PHONY: aaudio-seam
aaudio-seam: build/t_aaudio
	@./build/t_aaudio

# Writers against a device that is reopened at changing formats, closed,
# flushed and restarted under them, mixed and with one owner: no crash and no
# deadlock (a deadline per case). The lock order it holds to is described at
# the top of runtime/media/kl_audio.c. Opens the real output device for a few
# seconds (SKIPs without one); in `make check`. audio-race-asan is the same
# under AddressSanitizer.
build/t_audiorace: tests/t_audiorace.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_audiorace.c $(RUNTIME) $(LDLIBS)

.PHONY: audio-race audio-race-asan
audio-race: build/t_audiorace
	@./build/t_audiorace

build/t_audiorace_asan: tests/t_audiorace.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $@ tests/t_audiorace.c $(RUNTIME) $(LDLIBS)

audio-race-asan: build/t_audiorace_asan
	@./build/t_audiorace_asan

# The third: two streams MIXED (KL_AUDIO_MIX_SOURCES, Steam Link's shape — a
# float stream and an int16 stream at once). Each must deliver real time with
# no resyncs, and what reaches the device must be both. Opens the real output
# device for a few seconds, so it is NOT in `check` either.
build/t_audiomix: tests/t_audiomix.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_audiomix.c $(RUNTIME) $(LDLIBS)

.PHONY: audio-mix
audio-mix: build/t_audiomix
	@./build/t_audiomix

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

# UE's GameActivity virtual keyboard — the same gate for the other engine's
# keyboard. A missing AndroidThunkJava_ShowVirtualKeyboardInput aborts, and the
# obvious fix (a no-op) trades the abort for an editor the engine holds open
# forever, which produces no crash and no log line. No guest, no window, no
# headset; milliseconds, in `make check`.
build/t_uevk: tests/t_uevk.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_uevk.c $(RUNTIME) $(LDLIBS)

uevk: build/t_uevk
	./build/t_uevk

build/t_variadic: tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(LDLIBS)

vatest: build/t_variadic
	./build/t_variadic

# The JNI object model: a jobject a host-side static caches must outlive the
# local frame it was built in, and every erased spelling Unity's C# produces for
# a call must resolve to the same binding and the same object. Both failures
# abort a boot hundreds of lines from the binding at fault (see kl_jni.c's
# kl_jni_object_lifetime_selftest). No guest, no window, no headset;
# milliseconds, in `make check`.
build/t_jniobj: tests/t_jniobj.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jniobj.c $(RUNTIME) $(LDLIBS)

jniobj: build/t_jniobj
	./build/t_jniobj

# java.io.FileOutputStream and File.delete/setWritable/setReadOnly through the
# guest-facing JNI slots, A and V calls both, against a temporary directory
# under build/: what is written, what a failed open or write throws, which
# permission bits move, and that deleting a link leaves its target. No guest;
# in `make check`.
build/t_jni_io: tests/t_jni_io.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_io.c $(RUNTIME) $(LDLIBS)

.PHONY: jni-io
jni-io: build/t_jni_io
	./build/t_jni_io

# Which web pages the DreamSpace web window may show (kl_mono_web_url, also
# asked by its navigation delegate) and what an intent names
# (kl_jni_intent_web): https only, http upgraded, listed hosts only, plain DNS
# names only. Pure string checks; in `make check`.
build/t_jni_url: tests/t_jni_url.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_url.c $(RUNTIME) $(LDLIBS)

# BattleGroupVR2's own answers (fix 38), each with the proof that no other
# title gets them: the Firebase and Vivox starts that throw instead of
# stopping the process, and the Unity 6.2 memory callback with the title's
# memory defaults. Made-up SDK calls; no game, no network; in `make check`.
build/t_jni_firebase: tests/t_jni_firebase.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_firebase.c $(RUNTIME) $(LDLIBS)

build/t_jni_vivox: tests/t_jni_vivox.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_vivox.c $(RUNTIME) $(LDLIBS)

# A UE 4.21 guest's boot surface (Robo Recall: Unplugged): the AConfiguration
# getters native_app_glue prints, zlib's compress(), MediaPlayer14(ZZ)V.
build/t_ue421: tests/t_ue421.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_ue421.c $(RUNTIME) $(LDLIBS)

.PHONY: ue421
ue421: build/t_ue421
	./build/t_ue421

build/t_jni_memory: tests/t_jni_memory.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_memory.c $(RUNTIME) $(LDLIBS)

# Arizona Sunshine 2's own answers (Vertigo's logcat read, the Sentry Java SDK
# dropped), and that every other title still stops on those names.
build/t_jni_azs2: tests/t_jni_azs2.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_jni_azs2.c $(RUNTIME) $(LDLIBS)

.PHONY: jni-firebase jni-vivox jni-memory jni-azs2
jni-firebase: build/t_jni_firebase
	./build/t_jni_firebase
jni-vivox: build/t_jni_vivox
	./build/t_jni_vivox
jni-memory: build/t_jni_memory
	./build/t_jni_memory
jni-azs2: build/t_jni_azs2
	./build/t_jni_azs2

# Gun Club VR's quit teardown and preference-file trace, and that every other
# title keeps its frame loop and its silent file ops.
build/t_gunclub: tests/t_gunclub.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_gunclub.c $(RUNTIME) $(LDLIBS)

.PHONY: gunclub
gunclub: build/t_gunclub
	./build/t_gunclub

# "Max resolution per eye": the launch line's KL_OVRP_EYE_MAX over the player's
# choice over the title's cap over 3072, and Native uncapped.
build/t_maxeye: tests/t_maxeye.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_maxeye.c $(RUNTIME) $(LDLIBS)

.PHONY: maxeye
maxeye: build/t_maxeye
	./build/t_maxeye

# The installation's asset-file list (runtime/xr/kl_assetcfg.c): which lines
# are entries, which are refused (traversal, paths out of the OBB directory,
# malformed numbers), and installed only at exactly the listed size. Built from
# that one file — it depends on nothing else in the runtime.
build/t_assetcfg: tests/t_assetcfg.c runtime/xr/kl_assetcfg.c runtime/xr/kl_assetcfg.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_assetcfg.c runtime/xr/kl_assetcfg.c

.PHONY: assetcfg
assetcfg: build/t_assetcfg
	./build/t_assetcfg

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

# klepton-ld's SPLIT veneer placement (two pools, per-slice choice, halving that
# keeps odd-word tails instruction-aligned) on a synthetic sparse library wider
# than two `b` windows — no host tree carries one (Deadpool VR's libUE4 is the
# real case). Also run by `make check`.
.PHONY: ldsplit
ldsplit: build/klepton-ld
	python3 tests/t_ld_split.py build/klepton-ld build/ldsplit

x18: build/t_x18
	python3 tools/check_x18.py build/t_x18 $(foreach f,$(GUEST_LIBS),$(LIBS)/$(f).so)

# The `br x18` veneer's race windows, manufactured and executed so the fix can
# be checked somewhere other than a headset. Seeds the TSD slot through
# kl_thread_init, so it links the runtime.
build/t_x18race: tests/t_x18race.c $(RUNTIME) runtime/kl_x18.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_x18race.c $(RUNTIME) $(LDLIBS)

x18race: build/t_x18race
	./build/t_x18race

# The CTR_EL0 veneer, EXECUTED. Same shape as t_x18: linked against
# the decoder alone, so a veneer bug cannot hide behind a working loader. It
# runs the real illegal instruction in a child as its control, which is why the
# host can gate a crash that only ever happened on a headset.
build/t_ctr: tests/t_ctr.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_ctr.c runtime/kl_x18.c runtime/kl_env.c

ctr: build/t_ctr
	./build/t_ctr

# The guest-mutex table under create/destroy churn (kl_pthread.c). The failure
# it guards is a game that stops in a loading screen with every thread waiting,
# on a headset, now and then; here it is a sequence of six calls.
build/t_mtxtable: tests/t_mtxtable.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_mtxtable.c $(RUNTIME) $(LDLIBS)

mtxtable: build/t_mtxtable
	./build/t_mtxtable

# The Linux ABI's veneers (thread pointer and svc), EXECUTED, on the same terms.
build/t_lx: tests/t_lx.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_lx.c runtime/kl_x18.c runtime/kl_env.c

.PHONY: lx
lx: build/t_lx
	./build/t_lx

# The data window's two-entry literal-pool rule and its scope (one target),
# on real PhysX and BoringSSL words. Decoder only, like t_x18. In `make check`.
build/t_x18pool: tests/t_x18pool.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_x18pool.c runtime/kl_x18.c runtime/kl_env.c

.PHONY: x18pool
x18pool: build/t_x18pool
	./build/t_x18pool

# CASP with x18:x19 as one of its register pairs, EXECUTED (KLX_HZ_PAIR).
build/t_casp: tests/t_casp.c runtime/kl_x18.c runtime/kl_x18.h runtime/kl_env.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_casp.c runtime/kl_x18.c runtime/kl_env.c

.PHONY: casp
casp: build/t_casp
	./build/t_casp

# ---- the Linux process runtime (runtime/linux/) ----
# A glibc guest's own loader and libc, translated and run. LX_RT is the sysroot
# they come from — Valve's Steam Linux Runtime, unpacked beside this checkout.
LX_RT   ?= ../steamrt/root/files/lib/aarch64-linux-gnu
# The guest's root: the runtime tarball's `files` is its /usr, and /lib, /bin and
# /etc are the usual merged-usr aliases of it.
LX_ROOT ?= ../steamrt/root/files
LX_RUN   = ./build/m_linux --mount /usr=$(LX_ROOT) --mount /lib=$(LX_ROOT)/lib \
           --mount /bin=$(LX_ROOT)/bin --mount /etc=$(LX_ROOT)/etc --dylibs build/lx

build/m_linux: mains/m_linux.c runtime/linux/kl_lx.h $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ mains/m_linux.c $(RUNTIME) $(LDLIBS)

# Valve's Fossilize (vendor-fossilize/, MIT) and the recorder/replayer built on
# it, runtime/gfx/kl_fossilize.cpp (KL_VK_FOZ_RECORD / KL_VK_FOZ_REPLAY): the
# only C++ in the runtime, compiled into the same archives. Whatever links an
# archive takes the C++ library with it (-lc++); the host mains, which are one
# cc command over the C sources, link without it and kl_vulkan.c finds the
# recorder absent (a weak reference).
FZ_DIR := vendor-fossilize
FZ_SRC := $(FZ_DIR)/fossilize.cpp $(FZ_DIR)/fossilize_db.cpp $(FZ_DIR)/varint.cpp $(FZ_DIR)/path.cpp \
          $(FZ_DIR)/fossilize_application_filter.cpp runtime/gfx/kl_fossilize.cpp
FZ_DEPS := $(FZ_SRC) $(FZ_DIR)/miniz/miniz.c $(wildcard $(FZ_DIR)/*.hpp) runtime/gfx/kl_vk_pipe_hooks.h
FZ_INC := -I$(FZ_DIR) -I$(FZ_DIR)/rapidjson/include -I$(FZ_DIR)/khronos -I$(FZ_DIR)/miniz -Iruntime/gfx
FZ_CXX := clang++ -std=c++17 -g -O2 -arch arm64 -fno-exceptions -Wno-unused-parameter -Wno-deprecated-declarations $(FZ_INC)
# $(1) = platform directory, $(2) = its target flags
define FZ_COMPILE
	@for s in $(FZ_SRC); do \
	  $(FZ_CXX) $(2) -c $$s -o build/$(1)/fz_$$(basename $$s .cpp).o || exit 1; done
	@clang -g -O2 -arch arm64 $(2) -c $(FZ_DIR)/miniz/miniz.c -o build/$(1)/fz_miniz.o
endef

# The pipeline replay on this Mac, with no guest (tests/t_foz.c). The one host
# binary with the C++ recorder in it.
build/t_foz: tests/t_foz.c $(RUNTIME) $(RUNTIME_HDRS) $(FZ_DEPS)
	@mkdir -p build/fzhost
	@for s in $(FZ_SRC); do \
	  $(FZ_CXX) -c $$s -o build/fzhost/fz_$$(basename $$s .cpp).o || exit 1; done
	@clang -g -O2 -arch arm64 -c $(FZ_DIR)/miniz/miniz.c -o build/fzhost/fz_miniz.o
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tests/t_foz.c $(RUNTIME) build/fzhost/*.o $(LDLIBS) -lc++

# m_boot with the recorder in it — to collect a guest's pipelines on this Mac
# (KL_VK_FOZ_DIR + a guest that can be walked through its content without a
# person: KL_PROBE_CALL). The objects are t_foz's.
build/m_boot_fz: build/m_boot build/t_foz
	$(CC) $(CFLAGS) -fobjc-arc $(shell pkg-config --cflags sdl3) -o $@ \
	  mains/m_boot.c tests/t_mtl_provider.m $(RUNTIME) runtime/gfx/kl_view.c \
	  runtime/gfx/kl_view_mtl.m build/fzhost/*.o \
	  $(LDLIBS) -lc++ -framework Metal -framework QuartzCore -framework Foundation \
	  $(shell pkg-config --libs sdl3)

# The whole sysroot, ad-hoc signed — Apple silicon maps no unsigned code even on
# the host. A few hundred libraries; tools/lx_translate.sh runs them in parallel.
.PHONY: lx-rt lx-boot
lx-rt: build/klepton-ld
	@tools/lx_translate.sh build/lx $(LX_RT)

# The first rung: glibc's own dynamic loader, run as a program.
lx-boot: build/m_linux lx-rt
	$(LX_RUN) /lib/ld-linux-aarch64.so.1 --version

# The broadcast fan-out, driven through the shim's own sendto and aimed
# at loopback, so it exercises the discovery path without putting 254 datagrams
# on anyone's network. The path is otherwise reachable only from a live Qt
# frontend with a Steam host on the LAN.
build/t_bcast: tests/t_bcast.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_bcast.c $(RUNTIME) $(LDLIBS)

bcast: build/t_bcast
	./build/t_bcast

# What the guest reads in errno after a failed send/recv (see the test's header):
# the shim's own recv, send and __errno, on local sockets.
build/t_neterrno: tests/t_neterrno.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_neterrno.c $(RUNTIME) $(LDLIBS)

neterrno: build/t_neterrno
	./build/t_neterrno

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

# The foveation tests (kl_fove.m, docs/FOVEATION.md): profile maths and tables,
# plus GPU self-tests that skip themselves without a Metal device.
build/t_fove: tests/t_fove.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_fove.c $(RUNTIME) $(LDLIBS)

# The metrics panel's GPU numbers (kl_mtl_busy.m): every committed command
# buffer counted, overlapping spans once. Skips itself without a Metal device.
build/t_mtlbusy: tests/t_mtlbusy.m runtime/gfx/kl_mtl_busy.m runtime/gfx/kl_mtl_busy.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tests/t_mtlbusy.m runtime/gfx/kl_mtl_busy.m -framework Metal -framework Foundation

# ...and the sample they are part of (kl_metrics.c): rates, the busiest thread, memory.
build/t_metrics: tests/t_metrics.c runtime/diag/kl_metrics.c runtime/diag/kl_metrics.h runtime/gfx/kl_mtl_busy.m runtime/gfx/kl_mtl_busy.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ tests/t_metrics.c runtime/diag/kl_metrics.c runtime/gfx/kl_mtl_busy.m -framework Metal -framework Foundation

# The metrics panel for the macOS viewer (KL_STATS_CARD with --view): the same drawing as a dylib that
# kl_view_mtl.m loads from beside m_boot only when the panel is asked for. Not in `check`; m_boot does
# not depend on it.
build/libklvm_panel.dylib: tools/viewer_panel/panel.swift visionos/Sources/KleptonMetrics.swift runtime/diag/kl_metrics.h
	@mkdir -p build
	swiftc -O -emit-library -o $@ -import-objc-header runtime/diag/kl_metrics.h \
	  visionos/Sources/KleptonMetrics.swift tools/viewer_panel/panel.swift

.PHONY: viewer-panel
viewer-panel: build/libklvm_panel.dylib

# The panel itself as a picture, from a made-up minute: the drawing runs on the
# Mac, so its layout can be looked at without a headset.
build/metrics_preview: tools/metrics_preview/main.swift visionos/Sources/KleptonMetrics.swift
	@mkdir -p build
	swiftc -O -o $@ visionos/Sources/KleptonMetrics.swift tools/metrics_preview/main.swift

# The room on the floor (kl_room.c): scan map, suggested play area, drawn areas,
# distance to the edge, finding a room measured before — on a made-up room.
build/t_room: tests/t_room.c runtime/xr/kl_room.c runtime/xr/kl_room.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_room.c runtime/xr/kl_room.c

.PHONY: room
room: build/t_room
	./build/t_room

# Hand tracking for OVRPlugin guests (kl_hands.c): synthetic ARKit hands in,
# OVR's 24-bone hand skeleton out — bind pose, mirror relation, finger bend,
# pinch, finding ARKit's joint axes.
# BRINK Traveler: its depth through the real reprojection shader, a pinch as a tap or a swipe, and
# ARKit's joints as OpenXR's. No guest and no headset.
build/t_brink_depth: tests/t_brink_depth.m tests/t_reproject.m runtime/gfx/kl_reproject.c \
                     runtime/gfx/kl_reproject.h runtime/kl_env.c runtime/xr/kl_ovrp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< runtime/gfx/kl_reproject.c \
	  runtime/kl_env.c -framework Metal -framework Foundation

build/t_brink_selection: visionos/Sources/KleptonBrinkSelection.swift tests/brink-selection/main.swift
	@mkdir -p build
	swiftc -o $@ $^

build/t_brink_hands: visionos/Sources/KleptonBrinkSelection.swift tests/brink-hands/main.swift
	@mkdir -p build
	swiftc -o $@ $^

.PHONY: brink
brink: build/t_brink_depth build/t_brink_selection build/t_brink_hands
	./build/t_brink_depth
	./build/t_brink_selection
	./build/t_brink_hands

build/t_hands: tests/t_hands.c runtime/xr/kl_hands.c runtime/xr/kl_hands.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_hands.c runtime/xr/kl_hands.c

.PHONY: hands
hands: build/t_hands
	./build/t_hands

# The discrete hand pinch (KleptonPinch.swift, KL_PINCH_DISCRETE): the filter
# the app compiles, run on the Mac against timed sequences of fingertip values
# and system pinch events at 60, 90 and 100 Hz. No ARKit; in `make check`.
build/t_pinch: tests/t_pinch.swift visionos/Sources/KleptonPinch.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonPinch.swift tests/t_pinch.swift

# Hardware-independent tests for the actual shipped Surreal decoder, mapper and fusion.
SURREAL_CORE := visionos/Sources/KleptonControllerInput.swift visionos/Sources/KleptonControllerMode.swift visionos/Sources/KleptonSurrealInput.swift visionos/Sources/KleptonSurrealAim.swift
build/t_surreal: $(SURREAL_CORE) tests/t_surreal.swift
	@mkdir -p build
	swiftc -o $@ $(SURREAL_CORE) tests/t_surreal.swift

.PHONY: surreal
surreal: build/t_surreal
	./build/t_surreal
	python3 tests/t_surreal_project.py

.PHONY: pinch
pinch: build/t_pinch
	./build/t_pinch

# The MetalFX switch and the resolution (KleptonMetalFXPreset.swift): on at
# 100 % sets 75 %, off gives 100 % back only if the switch lowered it and the
# slider was not moved since; the mark survives a new start. In `make check`.
build/t_mfxpreset: tests/t_mfxpreset.swift visionos/Sources/KleptonMetalFXPreset.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonMetalFXPreset.swift tests/t_mfxpreset.swift

.PHONY: mfxpreset
mfxpreset: build/t_mfxpreset
	./build/t_mfxpreset

# MetalFX in mixed reality (KleptonMetalFXAlpha.swift): the real spatial scaler
# on the Mac's GPU, then the alpha pass, for every format the compositor
# upscales — transparent stays transparent, opaque stays opaque, one clean edge.
# In `make check`; says SKIP on a Mac without the scaler.
build/t_mfxalpha: tests/t_mfxalpha.swift visionos/Sources/KleptonMetalFXAlpha.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonMetalFXAlpha.swift tests/t_mfxalpha.swift

.PHONY: mfxalpha
mfxalpha: build/t_mfxalpha
	./build/t_mfxalpha

# The metrics panel over the guest's quad layers (KleptonOverlayDepth.swift): the composite's quad pass
# on the Mac's GPU with the real overlay shader and panel — a quad nearer than the panel (Alien's levels)
# must not hide it. Writes {menu,level}-{before,after}.png to $$T_STATSCARD_SHOTS or build/.
build/t_statscard: tests/t_statscard.swift tests/t_statscard.h visionos/Sources/KleptonOverlayDepth.swift \
                   visionos/Sources/KleptonMetrics.swift runtime/gfx/kl_reproject.c runtime/gfx/kl_reproject.h \
                   runtime/kl_env.c runtime/xr/kl_ovrp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o build/t_statscard_reproject.o runtime/gfx/kl_reproject.c
	$(CC) $(CFLAGS) -c -o build/t_statscard_env.o runtime/kl_env.c
	swiftc -o $@ -import-objc-header tests/t_statscard.h $(RUNTIME_INC) \
	  visionos/Sources/KleptonOverlayDepth.swift visionos/Sources/KleptonMetrics.swift tests/t_statscard.swift \
	  build/t_statscard_reproject.o build/t_statscard_env.o

.PHONY: statscard
statscard: build/t_statscard
	./build/t_statscard

# Half-Life: Alyx's loading screen, from the game's own models (KleptonHologram.swift), on the Mac's GPU.
# Needs T_ALYX_HLVR, the game's hlvr folder; without it the test says so and passes.
build/t_hologram: tests/t_hologram.swift visionos/Sources/KleptonHologram.swift
	@mkdir -p build
	swiftc -O -o $@ visionos/Sources/KleptonHologram.swift tests/t_hologram.swift

.PHONY: hologram
hologram: build/t_hologram
	./build/t_hologram

# "Max resolution per eye" in the start window (KleptonEyeMax.swift): the
# options, the stored choice per game, and the old Test switch taken over once.
build/t_maxeye_swift: tests/t_maxeye.swift visionos/Sources/KleptonEyeMax.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonEyeMax.swift tests/t_maxeye.swift

.PHONY: maxeyeswift
maxeyeswift: build/t_maxeye_swift
	./build/t_maxeye_swift
# Walkabout's "unlock all putters and balls" (KleptonWalkaboutSave.swift) on a
# COPY of a real save: android-files/files of the game and its
# global-metadata.dat. Not in `make check` — it needs a player's save.
#   make walkaboutunlock WB_FILES=<copy>/files WB_META=<tree>/assets/bin/Data/Managed/Metadata/global-metadata.dat
build/t_walkabout_unlock: tests/t_walkabout_unlock.swift visionos/Sources/KleptonWalkaboutSave.swift
	@mkdir -p build
	swiftc -O -parse-as-library -o $@ visionos/Sources/KleptonWalkaboutSave.swift tests/t_walkabout_unlock.swift

.PHONY: walkaboutunlock
walkaboutunlock: build/t_walkabout_unlock
	./build/t_walkabout_unlock "$(WB_FILES)" "$(WB_META)"

# Which builds are test builds (KleptonTestBuild.swift; KLTestBuild, which
# gen_xcodeproj.py bakes): what only a tester sees must never reach a
# player's build.
build/t_testbuild: tests/t_testbuild.swift visionos/Sources/KleptonTestBuild.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonTestBuild.swift tests/t_testbuild.swift

.PHONY: testbuild
testbuild: build/t_testbuild
	./build/t_testbuild
	python3 tests/t_testbuild_gen.py
	python3 tests/t_steam_art.py
	python3 tests/t_steam_art.py

# The compositor's array-view cache (KleptonArrayViews.swift): 25 per-eye
# swapchain rebuilds on the Mac's Metal device; the released images must be
# freed and the device's allocated size stay flat. In `make check`.
build/t_arrayviews: tests/t_arrayviews.swift visionos/Sources/KleptonArrayViews.swift
	@mkdir -p build
	swiftc -o $@ visionos/Sources/KleptonArrayViews.swift tests/t_arrayviews.swift

.PHONY: arrayviews
arrayviews: build/t_arrayviews
	./build/t_arrayviews

.PHONY: metrics-preview
metrics-preview: build/metrics_preview
	@./build/metrics_preview 60 build/metrics && echo "  build/metrics-panel.png, build/metrics-line.png," \
	  "build/metrics-panel-notap.png, build/metrics-panel-tap.png"

# Full regression sweep — run this first when picking work back up.
#
# tgkill/tkill(tid, 0) liveness probes answered by the raw-syscall door.
build/t_tgkill: tests/t_tgkill.c $(RUNTIME)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_tgkill.c $(RUNTIME) $(LDLIBS)

.PHONY: tgkill
tgkill: build/t_tgkill
	./build/t_tgkill

# Each test writes to a log and is checked BEFORE the log is filtered. Piping a
# test straight into tail/grep would hand make the filter's exit status instead
# of the test's, so a failing test would leave the sweep green.
check: build/t_futex build/t_opus build/t_variadic build/t_load build/t_il2cpp build/m_boot build/t_haptics build/t_hevc build/t_xrspace build/t_ovrplat build/t_posebound build/t_ovrplayers build/t_trackers build/t_untested build/t_audio build/t_audiorace build/t_xrinput build/t_softinput build/t_ctr build/t_mtxtable build/t_bcast build/t_neterrno build/t_fault build/t_glcaps build/t_x18race build/t_tgkill build/klepton-ld build/t_uevk build/t_jniobj build/t_fove build/t_mtlbusy build/t_metrics build/t_dl_ext build/t_jni_io build/t_jni_url build/t_jni_firebase build/t_jni_vivox build/t_jni_memory build/t_jni_azs2 build/t_gunclub build/t_maxeye build/t_assetcfg build/t_pinch build/t_arrayviews build/t_surreal build/t_mfxpreset build/t_mfxalpha build/t_statscard build/t_hologram build/t_maxeye_swift build/t_x18pool build/t_ue421 build/t_room build/t_hands build/t_brink_depth build/t_brink_selection build/t_brink_hands
	@# The guest steps read $(TARGET)'s own files. Without them the loader steps print nothing and pass, and the
	@# run ends at the il2cpp step with a bare "Error 1" - a check that looked green until its last line. A
	@# working copy without the game's tree (a fresh git worktree) is told so here, before anything runs.
	@test -f $(LIBS)/libil2cpp.so || { echo "check needs the guest it tests: $(LIBS)/libil2cpp.so is not here."; \
	  echo "  Unpack $(TARGET) into this working copy (see BUILDING.md), link or clone the tree from another one,"; \
	  echo "  or name a guest that is here: make check TARGET=<name>"; exit 1; }
	@./build/t_surreal
	@echo "=== variadic ABI ===" && ./build/t_variadic
	@./build/t_x18race > build/x18race.log 2>&1 || { cat build/x18race.log; exit 1; }
	@head -1 build/x18race.log && tail -1 build/x18race.log
	@./build/t_x18pool > build/x18pool.log 2>&1 || { cat build/x18pool.log; exit 1; }
	@head -1 build/x18pool.log && tail -1 build/x18pool.log
	@./build/t_tgkill > build/tgkill.log 2>&1 || { cat build/tgkill.log; exit 1; }
	@grep -E '=== tgkill' build/tgkill.log && tail -1 build/tgkill.log
	@python3 tests/t_ld_split.py build/klepton-ld build/ldsplit > build/ldsplit.log 2>&1 || { cat build/ldsplit.log; exit 1; }
	@echo "=== klepton-ld SPLIT placement (synthetic 300 MB library) ===" && tail -1 build/ldsplit.log
	@echo "=== crash reporter ===" && ./build/t_fault
	@echo "=== GL limits vs ANGLE ===" && ./build/t_glcaps vendor/out/Debug
	@./build/t_hevc > build/hevc.log 2>&1 || { cat build/hevc.log; exit 1; }
	@grep -E '=== HEVC|30 access units' build/hevc.log && tail -1 build/hevc.log
	@./build/t_xrspace > build/xrspace.log 2>&1 || { cat build/xrspace.log; exit 1; }
	@head -2 build/xrspace.log && tail -1 build/xrspace.log
	@./build/t_ovrplat > build/ovrplat.log 2>&1 || { cat build/ovrplat.log; exit 1; }
	@head -1 build/ovrplat.log && tail -1 build/ovrplat.log
	@./build/t_posebound > build/posebound.log 2>&1 || { cat build/posebound.log; exit 1; }
	@head -1 build/posebound.log && grep -E '^(PASS|FAIL)' build/posebound.log
	@./build/t_ovrplayers > build/ovrplayers.log 2>&1 || { cat build/ovrplayers.log; exit 1; }
	@cat build/ovrplayers.log
	@./build/t_trackers > build/trackers.log 2>&1 || { cat build/trackers.log; exit 1; }
	@tail -1 build/trackers.log
	@./build/t_untested > build/untested.log 2>&1 || { cat build/untested.log; exit 1; }
	@head -1 build/untested.log && tail -1 build/untested.log
	@./build/t_audio > build/audio.log 2>&1 || { cat build/audio.log; exit 1; }
	@echo "=== audio sink: sample formats, OpenSL sources ===" && tail -1 build/audio.log
	@./build/t_audiorace > build/audiorace.log 2>&1 || { cat build/audiorace.log; exit 1; }
	@head -1 build/audiorace.log && tail -1 build/audiorace.log
	@./build/t_xrinput > build/xrinput.log 2>&1 || { cat build/xrinput.log; exit 1; }
	@grep -E '=== OpenXR actions' build/xrinput.log && tail -1 build/xrinput.log
	@./build/t_softinput > build/softinput.log 2>&1 || { cat build/softinput.log; exit 1; }
	@head -1 build/softinput.log && tail -1 build/softinput.log
	@./build/t_jniobj > build/jniobj.log 2>&1 || { cat build/jniobj.log; exit 1; }
	@head -1 build/jniobj.log && tail -1 build/jniobj.log
	@./build/t_uevk > build/uevk.log 2>&1 || { cat build/uevk.log; exit 1; }
	@head -1 build/uevk.log && tail -1 build/uevk.log
	@./build/t_ctr > build/ctr.log 2>&1 || { cat build/ctr.log; exit 1; }
	@head -2 build/ctr.log && tail -1 build/ctr.log
	@./build/t_mtxtable > build/mtxtable.log 2>&1 || { cat build/mtxtable.log; exit 1; }
	@head -1 build/mtxtable.log && tail -1 build/mtxtable.log
	@./build/t_bcast > build/bcast.log 2>&1 || { cat build/bcast.log; exit 1; }
	@head -1 build/bcast.log && tail -1 build/bcast.log
	@./build/t_neterrno > build/neterrno.log 2>&1 || { cat build/neterrno.log; exit 1; }
	@head -1 build/neterrno.log && tail -1 build/neterrno.log
	@./build/t_haptics > build/haptics.log 2>&1 || { cat build/haptics.log; exit 1; }
	@head -3 build/haptics.log && tail -1 build/haptics.log
	@./build/t_fove > build/fove.log 2>&1 || { cat build/fove.log; exit 1; }
	@head -1 build/fove.log && tail -1 build/fove.log
	@./build/t_mtlbusy > build/mtlbusy.log 2>&1 || { cat build/mtlbusy.log; exit 1; }
	@head -1 build/mtlbusy.log && tail -1 build/mtlbusy.log
	@./build/t_metrics > build/metrics.log 2>&1 || { cat build/metrics.log; exit 1; }
	@head -1 build/metrics.log && tail -1 build/metrics.log
	@KL_FUTEX_EXACT=1 ./build/t_futex > build/futex.log 2>&1 || { cat build/futex.log; exit 1; }
	@./build/t_dl_ext > build/dl-ext.log 2>&1 || { cat build/dl-ext.log; exit 1; }
	@echo "=== android_dlopen_ext ===" && tail -1 build/dl-ext.log
	@./build/t_jni_io > build/jni-io.log 2>&1 || { cat build/jni-io.log; exit 1; }
	@echo "=== JNI file output ===" && tail -1 build/jni-io.log
	@./build/t_jni_url > build/jni-url.log 2>&1 || { cat build/jni-url.log; exit 1; }
	@echo "=== JNI web addresses ===" && tail -1 build/jni-url.log
	@./build/t_jni_firebase > build/jni-firebase.log 2>&1 || { cat build/jni-firebase.log; exit 1; }
	@./build/t_jni_vivox > build/jni-vivox.log 2>&1 || { cat build/jni-vivox.log; exit 1; }
	@./build/t_jni_memory > build/jni-memory.log 2>&1 || { cat build/jni-memory.log; exit 1; }
	@echo "=== BattleGroupVR2: Firebase, Vivox, memory ===" && tail -1 build/jni-firebase.log && tail -1 build/jni-vivox.log && tail -1 build/jni-memory.log
	@./build/t_jni_azs2 > build/jni-azs2.log 2>&1 || { cat build/jni-azs2.log; exit 1; }
	@echo "=== Arizona Sunshine 2: logcat, Sentry ===" && tail -1 build/jni-azs2.log
	@./build/t_gunclub > build/gunclub.log 2>&1 || { cat build/gunclub.log; exit 1; }
	@head -1 build/gunclub.log && tail -1 build/gunclub.log
	@./build/t_maxeye > build/maxeye.log 2>&1 || { cat build/maxeye.log; exit 1; }
	@head -1 build/maxeye.log && tail -1 build/maxeye.log
	@./build/t_assetcfg > build/assetcfg.log 2>&1 || { cat build/assetcfg.log; exit 1; }
	@head -1 build/assetcfg.log && tail -1 build/assetcfg.log
	@./build/t_pinch > build/pinch.log 2>&1 || { cat build/pinch.log; exit 1; }
	@echo "=== discrete hand pinch ===" && tail -1 build/pinch.log
	@./build/t_room > build/room.log 2>&1 || { cat build/room.log; exit 1; }
	@echo "=== room and play area ===" && tail -1 build/room.log
	@./build/t_brink_depth > build/brink-depth.log 2>&1 || { cat build/brink-depth.log; exit 1; }
	@./build/t_brink_selection > build/brink-selection.log 2>&1 || { cat build/brink-selection.log; exit 1; }
	@./build/t_brink_hands > build/brink-hands.log 2>&1 || { cat build/brink-hands.log; exit 1; }
	@echo "=== BRINK Traveler: depth, selection, hand frames ===" && tail -1 build/brink-hands.log
	@./build/t_hands > build/hands.log 2>&1 || { cat build/hands.log; exit 1; }
	@echo "=== hand tracking (OVR skeleton) ===" && tail -1 build/hands.log
	@./build/t_arrayviews > build/arrayviews.log 2>&1 || { cat build/arrayviews.log; exit 1; }
	@echo "=== compositor array views ===" && tail -1 build/arrayviews.log
	@./build/t_ue421 > build/ue421.log 2>&1 || { cat build/ue421.log; exit 1; }
	@head -1 build/ue421.log && tail -1 build/ue421.log
	@./build/t_mfxpreset > build/mfxpreset.log 2>&1 || { cat build/mfxpreset.log; exit 1; }
	@echo "=== MetalFX switch: resolution preset ===" && tail -1 build/mfxpreset.log
	@./build/t_mfxalpha > build/mfxalpha.log 2>&1 || { cat build/mfxalpha.log; exit 1; }
	@head -1 build/mfxalpha.log && tail -1 build/mfxalpha.log
	@./build/t_statscard > build/statscard.log 2>&1 || { cat build/statscard.log; exit 1; }
	@./build/t_hologram > build/hologram.log 2>&1 || { cat build/hologram.log; exit 1; }
	@head -1 build/statscard.log && tail -1 build/statscard.log
	@./build/t_maxeye_swift > build/maxeye-swift.log 2>&1 || { cat build/maxeye-swift.log; exit 1; }
	@head -1 build/maxeye-swift.log && tail -1 build/maxeye-swift.log
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
# libil2cpp.so exists only on Unity IL2CPP titles. A UE4, SDL or native guest
# has nothing to point t_il2cpp at, and failing here stopped every gate after
# it. Skip loudly, as the opus gate does: a missing library is a property of
# the guest, not a regression.
	@echo "=== il2cpp runtime ===" && if [ -f $(LIBS)/libil2cpp.so ]; then \
	  ./build/t_il2cpp $(LIBS)/libil2cpp.so $(TARGET)/assets/bin/Data/Managed > build/il2cpp.log && tail -4 build/il2cpp.log; \
	else echo "  SKIPPED: $(LIBS) has no libil2cpp.so (not a Unity IL2CPP title)."; \
	     echo "  The guest-entry gate below still runs its own entry points."; fi
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
XROS_CFLAGS := $(REPRO_FLAGS) $(SC2_DEFINES) -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 $(MVK_INC) $(RUNTIME_INC)

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
	@mkdir -p build/swift-module-cache
	@xcrun -sdk xros swiftc -typecheck -target arm64-apple-xros26.0 \
	  -module-cache-path build/swift-module-cache \
	  -import-objc-header visionos/Sources/Klepton-Bridging-Header.h \
	  $(RUNTIME_INC) visionos/Sources/*.swift visionos/SteamCloudKit/Sources/SteamCloudKit/*.swift \
	  > build/swiftcheck.log 2>&1 || { cat build/swiftcheck.log; exit 1; }; \
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
build/xros/libklepton.a: $(RUNTIME_SHIP) $(RUNTIME_ALL_HDRS) $(FZ_DEPS)
	@mkdir -p build/xros
	@rm -f $@ $(@D)/*.o
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xros/$$(basename $$s | sed 's/\.[cSm]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) \
	    -c $$s -o $$o || exit 1; done
	$(call FZ_COMPILE,xros,-target arm64-apple-xros1.0 -isysroot $(XROS_SDK))
	@ar rcs $@ build/xros/*.o

build/xrsim/libklepton.a: $(RUNTIME_SHIP) $(RUNTIME_ALL_HDRS) $(FZ_DEPS)
	@mkdir -p build/xrsim
	@rm -f $@ $(@D)/*.o
	@for s in $(RUNTIME_SHIP); do \
	  o=build/xrsim/$$(basename $$s | sed 's/\.[cSm]$$/.o/'); \
	  $(CC) $(XROS_CFLAGS) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) \
	    -c $$s -o $$o || exit 1; done
	$(call FZ_COMPILE,xrsim,-target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK))
	@ar rcs $@ build/xrsim/*.o

# The gate: link each archive into a dylib with -all_load, so a symbol that is
# declared in a header but absent from the platform fails the build even inside
# a function nothing calls yet.
build/xros/libklepton.dylib: build/xros/libklepton.a
	@$(CC) -target arm64-apple-xros1.0 -isysroot $(XROS_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -lc++ -framework AudioToolbox \
	   -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
	   -framework CoreFoundation -framework AVFoundation -framework Foundation -framework Metal $(SC2_FRAMEWORKS) \
	   -install_name @rpath/libklepton.dylib

build/xrsim/libklepton.dylib: build/xrsim/libklepton.a
	@$(CC) -target arm64-apple-xros1.0-simulator -isysroot $(XRSIM_SDK) -arch arm64 \
	   -dynamiclib -o $@ -Wl,-all_load $< -lz -lc++ -framework AudioToolbox \
	   -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface \
	   -framework CoreFoundation -framework AVFoundation -framework Foundation -framework Metal $(SC2_FRAMEWORKS) \
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
# Extra gn args for all three slices. The pinned toolchain's lld cannot read an
# Xcode 27 SDK — its .tbd files name an arm64e.x1 target that this LLVM's TAPI
# reader rejects as "unknown target", so nothing links against libSystem. There,
# use_lld=false links with Apple's ld instead.
#
# Detected rather than passed by hand, because visionos/run.sh re-runs
# `make angle-xros angle-xrsim` on every device build: a flag given once to
# `make angle-all` is gone on the next gn gen, and the relink fails.
ANGLE_GN_EXTRA ?= $(shell grep -qs 'arm64e\.x1' \
                    "$$(xcrun --sdk macosx --show-sdk-path)/usr/lib/libSystem.tbd" \
                    && echo use_lld=false)
.PHONY: angle-ios angle-ios-sim angle-xros
angle-ios: angle-fetch
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
	  gn gen out/ios --args='is_debug=false target_os="ios" target_cpu="arm64" \
	    target_environment="device" ios_enable_code_signing=false \
	    angle_enable_vulkan=false angle_enable_swiftshader=false use_lld=false use_custom_libcxx=false use_custom_libcxx_for_host=false treat_warnings_as_errors=false $(ANGLE_GN_EXTRA)' && \
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
	    angle_enable_vulkan=false angle_enable_swiftshader=false use_thin_archive=false $(ANGLE_GN_EXTRA)' && \
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
build/t_glcaps: tests/t_glcaps.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< $(RUNTIME) \
	  $(LDLIBS) -framework Metal -framework QuartzCore -framework Foundation

# Our advertised GL limits vs what the vendored ANGLE will honour. Needs
# `make angle-debug`; passes (as a skip) without it, so a bare checkout is clean.
build/t_fault: tests/t_fault.c $(RUNTIME) $(RUNTIME_HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< $(RUNTIME) \
	  $(LDLIBS) -framework Metal -framework QuartzCore -framework Foundation

# The crash reporter, on a process that really faults. Needs no guest.
fault: build/t_fault
	@./build/t_fault

glcaps: build/t_glcaps
	@./build/t_glcaps vendor/out/Debug

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
	cd vendor && if [ ! -f depot_tools/python3_bin_reldir.txt ]; then cd depot_tools && ./update_depot_tools && cd ..; fi
	cd vendor && export PATH="$$PWD/depot_tools:$$PATH" DEPOT_TOOLS_UPDATE=0 && \
		gn gen out/Debug --args='is_debug=true target_cpu="arm64" use_lld=false $(ANGLE_GN_EXTRA)' && \
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
# Surreal Interactive's visionOS SDK, the pose source of a Surreal Touch build.
# gen_xcodeproj.py fetches it by itself for every build that lacks it; this
# is for staging it ahead of time (a toolchain that ships with it, an offline
# build). Pinned and verified in the script; visionos/Frameworks/ is gitignored.
.PHONY: surreal-sdk
surreal-sdk:
	@./tools/surreal_sdk_fetch.sh

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
