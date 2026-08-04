CC      := clang
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64
LDLIBS  := -lz
RUNTIME := runtime/kl_image.c runtime/kl_shim.c runtime/kl_va.c \
           runtime/kl_va_handlers.c runtime/kl_va_thunks.S \
           runtime/kl_libc.c runtime/kl_pthread.c runtime/kl_dl.c \
           runtime/kl_ndk.c runtime/kl_jni.c runtime/kl_x18.c \
           runtime/kl_egl.c runtime/kl_opensl.c runtime/kl_ovrp.c

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

build/t_variadic: tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_variadic.c tests/t_variadic_call.S $(RUNTIME) $(LDLIBS)

vatest: build/t_variadic
	./build/t_variadic

build/t_boot: tests/t_boot.c $(RUNTIME) runtime/klepton.h runtime/kl_jni.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_boot.c $(RUNTIME) $(LDLIBS)

boot: build/t_boot
	./build/t_boot $(LIBS)

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
build/t_x18: tests/t_x18.c runtime/kl_x18.c runtime/kl_x18.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/t_x18.c runtime/kl_x18.c

x18: build/t_x18
	python3 tools/check_x18.py build/t_x18 $(LIBS)/libunity.so $(LIBS)/libil2cpp.so \
	  $(LIBS)/libunityopus.so $(LIBS)/libmain.so $(LIBS)/lib_burst_generated.so

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
check: build/t_opus build/t_variadic build/t_load build/t_il2cpp build/t_boot
	@echo "=== variadic ABI ===" && ./build/t_variadic
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
	@echo "=== guest entry (JNI_OnLoad, initJni) ===" && ./build/t_boot $(LIBS) > build/boot.log 2>/dev/null && \
	  grep -E 'JNI_OnLoad returned|registered com|load returned|natives registered:|ids requested:|M3 EXIT|M4 \(partial\)' build/boot.log
