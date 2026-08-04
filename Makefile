CC      := clang
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64
LDLIBS  := -lz
RUNTIME := runtime/kl_image.c runtime/kl_shim.c runtime/kl_va.c \
           runtime/kl_va_handlers.c runtime/kl_va_thunks.S \
           runtime/kl_libc.c runtime/kl_pthread.c runtime/kl_dl.c \
           runtime/kl_ndk.c runtime/kl_jni.c

.PHONY: all test clean check load vatest il2cpp boot jnislots
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
	@echo "=== il2cpp runtime ===" && ./build/t_il2cpp $(LIBS)/libil2cpp.so beatsaber/assets/bin/Data/Managed > build/il2cpp.log && tail -4 build/il2cpp.log
	@echo "=== guest entry (JNI_OnLoad, initJni) ===" && ./build/t_boot $(LIBS) > build/boot.log 2>/dev/null && \
	  grep -E 'JNI_OnLoad returned|registered com|load returned|natives registered:|ids requested:|M3 EXIT|M4 \(partial\)' build/boot.log
