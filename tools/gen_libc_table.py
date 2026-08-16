#!/usr/bin/env python3
"""Regenerate runtime/kl_libc_table.h — the direct bionic->Darwin forwards.

A symbol qualifies for a direct forward only if its signature AND every struct it
touches are identical on both platforms. Everything else is hand-written in
kl_libc.c / kl_pthread.c / kl_dl.c, or thunked in kl_va_thunks.S.
"""
import struct, re, sys, os, glob, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The Unity target is discovered twice over — which LIBRARIES (unity_libs below)
# and which TREES (here) — and both were a hardcoded list that went stale across
# a guest version swap.
#
# Libraries: a property of the APK. The 2019.4 build (Beat Saber 1.28) has
# lib_burst_generated and libunityopus; 1.6.0 (Unity 2018.4) has neither and
# adds libvrintegrationloader. The stale list simply stopped scanning the
# libraries the guest actually loads, so the twelve imports they need were never
# forwarded — surfacing as a batch of abort-by-name stops that read like guest
# changes rather than like a tooling list nobody updated.
#
# Trees: scanning only whichever version currently occupies beatsaber/ makes
# this table PING-PONG. Regenerating with 1.6.0 unpacked dropped fifteen names
# 1.28 imports (isgraph, iswdigit, frexpf, ldexpf, lldiv, logb, sigaltstack, …),
# which took `make check` down with 14 unresolved imports in libunity and 9 in
# libil2cpp — and regenerating it back would drop 1.6.0's seven in turn. Neither
# version is wrong, so the table is the UNION over every unpacked tree present:
# a forward costs one table entry, and a forward for a name this guest never
# imports costs nothing at all. Beat Saber alone spans Unity 2018.4 and 2019.4,
# so this is a standing property of the project, not a one-off.
#
# A machine holding fewer trees regenerates a SUBSET, which is the one way this
# can still lose a name. It is visible rather than silent — the scan prints
# every tree and library it found, and `make check` fails loudly on any
# unresolved import for whichever guest is installed.
#
# Which trees, in two parts, because there are two ways one gets here. The
# TARGETS in visionos/targets.py are named — every Unity guest this project
# builds an app for — and the version variants of one of them are not
# (`beatsaber-113/`, `-128/`, `-16/` are the same target at other versions, and
# the union above is exactly what makes them safe to keep unpacked). Asking the
# table rather than listing the targets keeps the third copy of it from
# existing; the glob then adds the versions beside each one.
def _unity_trees():
    trees = []
    sys.path.insert(0, os.path.join(ROOT, 'visionos'))
    try:
        import targets as _t
        named = [v['srcdir'] for v in _t.TARGETS.values() if v.get('kind') == 'unity']
    except Exception as e:                       # a checkout without the table
        print(f'  (targets.py unavailable ({e}); falling back to beatsaber*)',
              file=sys.stderr)
        named = ['beatsaber/lib/arm64-v8a']
    for src in named:
        tree = src.split('/')[0]
        trees += glob.glob(os.path.join(ROOT, tree + '*', 'lib', 'arm64-v8a'))
    return sorted(set(trees))


UNITY_TREES = _unity_trees()

# Both targets, and for Steam Link BOTH front doors — the streaming client and
# the 2D configuration frontend. See the third Steam Link entry for why the
# frontend is no longer excluded.
TARGETS = [(d, None) for d in UNITY_TREES] + [
    (os.path.join(ROOT, 'steamlink-android/lib/arm64-v8a'),
     ['libmain', 'libSDL3', 'libSDL3_ttf', 'libSDL3_image',
      'libh264bitstream', 'libhevcbitstream', 'libc++_shared']),
    # The VR build of the same app (steamlink-vr.apk, PLANNING §11.8). Its 2D
    # half is the same seven libraries; libvrlink_scene is the OpenXR
    # NativeActivity and is listed because it imports a different libc surface
    # (AAudio, more of the NDK) that has to reach the same shim.
    (os.path.join(ROOT, 'steamlink-vr/lib/arm64-v8a'),
     ['libmain', 'libSDL3', 'libSDL3_ttf', 'libSDL3_image',
      'libh264bitstream', 'libhevcbitstream', 'libc++_shared',
      'libvrlink_scene']),
    # ...and the app's OTHER front door: the 2D configuration frontend, which is
    # what SteamLink.getMainSharedObject() actually names. §11.2 declared libshell
    # and Qt out of scope, and the comment above used to say so; that was priced
    # against the wrong question. The frontend is the only half of this app with
    # pixels of its own — the streaming client draws nothing without a Steam host
    # on the LAN — and its libc surface turns out to be small (zlib, a few
    # Linux-only syscall wrappers) rather than "a 22 MB dependency nothing loads".
    #
    # libplugins_platforms_qvirtual is Valve's own Qt platform plugin and is
    # listed for the same reason libvrlink_scene is: it is dlopen'd rather than
    # DT_NEEDED'd, so nothing else would bring its imports into this set.
    (os.path.join(ROOT, 'steamlink-vr/lib/arm64-v8a'),
     ['libshell_arm64-v8a', 'libsteamwebrtc', 'libSDL3_mixer',
      'libQt6Core_arm64-v8a', 'libQt6Gui_arm64-v8a', 'libQt6Network_arm64-v8a',
      'libQt6Widgets_arm64-v8a', 'libQt6Svg_arm64-v8a',
      'libplugins_platforms_qvirtual_arm64-v8a']),
]

# Libraries we REPLACE rather than translate (PLANNING §3.1), so their imports
# are not ours to satisfy: libOVRPlugin needs Quest system libraries absent from
# any APK, libovrplatformloader is a forwarder to com.oculus.horizon, and
# libvrapi is never loaded because the chain terminates before it. Keep this in
# step with GUEST_REPLACED in the Makefile.
GUEST_REPLACED = {'libOVRPlugin', 'libovrplatformloader', 'libvrapi'}

# Not part of the APPLICATION at all, so its imports are not the guest's.
# Beat Saber 1.40 as unpacked here is a modified build whose
# UnityPlayerActivity.<clinit> was patched to System.loadLibrary("frda") -- a
# Frida-based injector, configured by libfrda.config.so to patch
# libovrplatformloader / libvrapi / libc and hijack entitlement RESPONSES for a
# table of 249 DLC asset IDs and ~270 SKUs. That is the circumvention this
# project refuses in code (kl_ovrplat.c, "The DRM line" in CLAUDE.md), and
# nothing in the game depends on it: no DT_NEEDED anywhere names either
# library. Keep in step with GUEST_EXCLUDED in the Makefile.
GUEST_EXCLUDED = {'libfrda', 'libscript'}


def is_elf(path):
    """A `.so` is not necessarily an ELF -- see unity_libs."""
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


def unity_libs(d):
    """Every translated guest library in an unpacked APK's lib dir.

    A TARGETS entry whose name list is None is resolved this way. Used for the
    Unity target because its library set is a Unity-version artifact rather
    than a property of this project -- pointing LIBS at another old-Unity title
    should not require editing a list here.

    Selected on the ELF MAGIC, not on the extension. Beat Saber 1.40 ships
    `libfrda.config.so`, which is JSON: naming a data file `.so` is how you get
    Android's installer to extract it into the app's lib dir beside the real
    libraries. Globbing on `.so` handed that file to undefined_symbols() below,
    which read its text as a section-header offset (8100131176265705836) and
    died inside struct.unpack -- an error naming neither the file nor the cause.
    """
    if not os.path.isdir(d):
        return []
    return sorted(f[:-3] for f in os.listdir(d)
                  if f.endswith('.so') and f[:-3] not in GUEST_REPLACED
                  and f[:-3] not in GUEST_EXCLUDED and is_elf(os.path.join(d, f)))


def undefined_symbols(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        sys.exit("gen_libc_table: %s is named .so but is not an ELF (%r...). A "
                 "TARGETS entry names it explicitly, so this is a stale list "
                 "rather than a discovery bug." % (os.path.relpath(path, ROOT), d[:8]))
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    ses, sn, _ = struct.unpack_from('<HHH', d, 0x3a)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * ses) for i in range(sn)]
    out = set()
    for s in sh:
        if s[1] != 11:                                  # SHT_DYNSYM
            continue
        st = sh[s[6]]
        sd = d[st[4]:st[4] + st[5]]
        for i in range(s[5] // 24):
            n, info, o, shndx, val, sz = struct.unpack_from('<IBBHQQ', d, s[4] + i * 24)
            if n and shndx == 0:
                out.add(sd[n:sd.index(b'\0', n)].decode())
    return out


def defined_symbols(path):
    """Dynamic symbols this library DEFINES — i.e. what its siblings bind to."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        sys.exit("gen_libc_table: %s is named .so but is not an ELF (%r...). A "
                 "TARGETS entry names it explicitly, so this is a stale list "
                 "rather than a discovery bug." % (os.path.relpath(path, ROOT), d[:8]))
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    ses, sn, _ = struct.unpack_from('<HHH', d, 0x3a)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * ses) for i in range(sn)]
    out = set()
    for s in sh:
        if s[1] != 11:                                  # SHT_DYNSYM
            continue
        st = sh[s[6]]
        sd = d[st[4]:st[4] + st[5]]
        for i in range(s[5] // 24):
            n, info, o, shndx, val, sz = struct.unpack_from('<IBBHQQ', d, s[4] + i * 24)
            if n and shndx != 0:
                out.add(sd[n:sd.index(b'\0', n)].decode())
    return out

# Hand-written elsewhere. Keep in sync with the E(...) entries in kl_shim.c.
SPECIAL = set("""
__errno environ gettid __system_property_find __system_property_get __system_property_read
prctl sched_getaffinity sched_setaffinity stat fstat lstat statfs uname sigaction
__FD_ISSET_chk __FD_SET_chk __ctype_get_mb_cur_max lseek64 getpwuid getpwuid_r
opendir readdir closedir setjmp longjmp strtold wcstold strtold_l
swprintf vprintf vsscanf execl system syscall sysconf fopen access mkdir unlink rename
dlopen dlsym dlclose dlerror dladdr dl_iterate_phdr
memrchr memalign
fputc fputs fwrite fread fclose fflush fgets getc feof ferror clearerr
fseek fseeko ftell ftello setvbuf
__memcpy_chk __memset_chk __strcpy_chk __strlen_chk __strchr_chk __vsnprintf_chk
__stack_chk_fail
clock_gettime clock_getres gettimeofday

getauxval fegetenv fesetenv feholdexcept feupdateenv statvfs fstatvfs sendfile
eventfd eventfd_read eventfd_write ppoll accept4 pipe2 dup3 memfd_create clone
inotify_init inotify_init1 inotify_add_watch inotify_rm_watch
__assert2 __FD_CLR_chk __fgets_chk __pthread_cleanup_push __pthread_cleanup_pop
epoll_create epoll_create1 epoll_ctl epoll_wait openat __open_2
__memmove_chk __strncpy_chk __strncpy_chk2 __strcat_chk __read_chk __vsprintf_chk
sincosf sincos putchar getchar fdatasync __cmsg_nxthdr __cxa_thread_atexit_impl
fileno fgetc ungetc getwc fgetwc ungetwc fputwc putwc fwide
stat64 lstat64 fstat64
stdout stderr stdin __register_atfork __gnu_strerror_r __write_chk _ctype_
dup2 mprotect
vsprintf
__google_potentially_blocking_region_begin __google_potentially_blocking_region_end
getrandom isnan
mmap madvise sysinfo
""".split())

# Excluded from the table but NOT hand-written anywhere: names we deliberately
# leave unresolved, so the loader's per-import trampoline aborts BY NAME if the
# guest ever calls one. That is the project's own rule — a lookup is a
# measurement, a call is an assertion — and an unresolved import already
# implements it for free.
#
# Kept separate from SPECIAL because SPECIAL is a PROMISE that kl_shim.c has an
# E(...) entry, and ptrace sat in it for a session with no implementation behind
# it. The next reader's obvious repair is to drop it back into the generated
# table, which is the one genuinely wrong answer available here: Linux's
# ptrace(enum __ptrace_request, ...) and Darwin's ptrace(int, pid_t, caddr_t,
# int) share a name and share almost no request numbers, so a direct forward
# would execute a DIFFERENT operation and report success (trap 6b's class).
#
# ptrace is Beat Saber 1.6.0's libunity only — an anti-debug probe, on no path
# 1.28 takes. Left unresolved rather than answered because nothing has yet
# forced a decision about what it should say, and inventing one is how trap 6d
# happens.
UNRESOLVED_BY_DESIGN = set("""
ptrace
""".split())

# Subsystem gateways with their own files, like gl*/SL_* below: AAudio is the
# VR build's audio API (kl_audio.c's sink already exists; the API in front of it
# does not), and xr* is the OpenXR runtime we must REPLACE rather than forward —
# libopenxr_loader.so talks to an Android runtime broker that does not exist
# here, exactly as libOVRPlugin.so did (PLANNING §3.1).
PREFIX_SUBSYSTEM = ('AAudio', 'xr')

# `_Z` is Itanium C++ mangling: never a libc forward, and never OURS to
# provide. Beat Saber 1.40's libunity weak-imports `_ZTH15gDeferredAction`, a
# clang TLS-init wrapper for a variable whose dynamic initializer was optimized
# away — a weak undefined that is CORRECT at NULL (kl_image.c honours that), and
# a generated forward for which names a symbol no Darwin library defines.
PREFIX_SKIP = ('pthread_', 'sem_', '__android_log', 'egl', '_Z')
# Same reason gl* is excluded: these are subsystem gateways with their own
# files, not libc. SL_/sl (OpenSL ES) -> kl_opensl.c, AMedia*/AMEDIA* ->
# kl_mediandk.c, SDL_/IMG_/TTF_ are the guest's own cross-library imports and
# bind against the guest libSDL3.
# The NDK families belong in kl_ndk.c, not here. This list grew when the VR
# build arrived: it reaches AHardwareBuffer/AImageReader (the decoder's
# zero-copy path), AKeyEvent/AMotionEvent (NativeActivity input) and ATrace,
# none of which exist on Darwin — and a generated forward for a symbol that
# does not exist is caught by the linker, which is the only reason this was not
# a silent wrong answer. Keep it generous: an over-skip costs one hand-written
# line, an under-skip costs a link error at best and trap 6b at worst.
RE_SKIP = re.compile(r'^(A[A-Z]|AMEDIA|SL_|sl[A-Z]|SDL_|IMG_|TTF_|Mix_|gl[A-Z])')

# Which platform refused a dropped name, for the header. Filled by main().
REASON = {}


def main():
    # No Unity tree at all would generate a Steam-Link-only table without
    # complaint — the same silent-drop failure the union above exists to stop,
    # in its largest possible form. beatsaber/ is the reference target.
    if not UNITY_TREES:
        sys.exit("gen_libc_table: no unpacked Unity guest found — expected at "
                 "least %s. Unpack the reference APK (BUILDING.md) before "
                 "regenerating, or this table loses every Unity import."
                 % os.path.relpath(os.path.join(ROOT, 'beatsaber/lib/arm64-v8a'), ROOT))
    print("Unity trees (the table is their UNION): %s" %
          ' '.join(os.path.relpath(d, ROOT).split(os.sep)[0] for d in UNITY_TREES))

    # PER TREE, and that is the whole point of the two dicts rather than two
    # sets. The union below is over (a tree's imports MINUS that tree's own
    # exports), because "a name a guest library exports is not a shim gap" is
    # only true INSIDE the tree that exports it.
    #
    # Getting this wrong is a silent cross-target regression and it happened:
    # VRChat ships libandroid-support.so — the NDK's compatibility library,
    # which defines the locale and wide-char functions old Android API levels
    # lack — and it exports towlower, mbsrtowcs and mbsnrtowcs. With one shared
    # `exported` set those three left the table for EVERY guest, so Beat Saber's
    # libil2cpp.so, whose tree contains nothing that defines them, went from 0
    # unresolved imports to 3. The build succeeded; only `make check`'s il2cpp
    # gate said so, and it said it about a title nobody had touched.
    per_tree = []            # [(imports, exports)], one entry per TARGETS row
    missing = []
    for libs, names in TARGETS:
        if names is None:
            names = unity_libs(libs)
            if not names:
                sys.exit("gen_libc_table: no guest libraries under %s" % libs)
            print("discovered %d guest librar%s in %s: %s" %
                  (len(names), 'y' if len(names) == 1 else 'ies',
                   os.path.relpath(libs, ROOT), ' '.join(names)))
        imports, exports = set(), set()
        for f in names:
            p = os.path.join(libs, f + '.so')
            if not os.path.exists(p):
                missing.append(p)
                continue
            imports |= undefined_symbols(p)
            exports |= defined_symbols(p)
        per_tree.append((imports, exports))
    # A named library that is not there means this list no longer describes the
    # unpacked APK — a guest version swap. Silence here regenerates a table
    # against the libraries that DO exist and drops every import unique to the
    # ones that do not, which surfaces much later as a batch of unrelated-looking
    # abort-by-name stops. Refuse instead: the list is a deliberate statement
    # about which libraries we translate, so it has to be edited, not inferred.
    if missing:
        sys.exit("gen_libc_table: %d target librar%s missing -- TARGETS is stale "
                 "for the unpacked APK:\n  %s" %
                 (len(missing), 'y is' if len(missing) == 1 else 'ies are',
                  '\n  '.join(missing)))
    # Nor is a name a library we REPLACE exports. libvrintegrationloader (1.6.0)
    # and libOVRMrcLib (1.11.1) link DIRECTLY against libvrapi, where 1.28's
    # chain only ever dlsym'd it — so scanning those trees put vrapi_* into this
    # set for the first time, and a generated forward would name a symbol that
    # does not exist on Darwin. The linker catches that, but only after the
    # question has been made confusing; these belong to kl_ovrp.c / kl_ovrplat.c,
    # or to nothing at all, since the chain terminates before libvrapi loads
    # (PLANNING §3.1). Read off the replaced libraries themselves rather than
    # listed, so a new Oculus family arriving with a future title is handled.
    #
    # This one IS global: a replaced library is replaced for every guest, and
    # the names belong to kl_ovrp.c / kl_ovrplat.c or to nothing at all.
    replaced = set()
    for d in UNITY_TREES:
        for name in sorted(GUEST_REPLACED):
            p = os.path.join(d, name + '.so')
            if os.path.exists(p):
                replaced |= defined_symbols(p)
    # A name one guest library exports is not a shim gap — libmain.so's 1418
    # unresolved import sites are overwhelmingly SDL3's, and they bind against
    # the guest libSDL3 at relocation time (kl_guest_sym_global). Generating a
    # forward for one would silently prefer the host's symbol of the same name.
    # Subtracted per tree, per the note above: a sibling in ANOTHER APK is not
    # something this guest's loader will ever bind against.
    allu = set()
    for imports, exports in per_tree:
        allu |= imports - exports
    allu -= replaced
    shim = open(os.path.join(ROOT, 'runtime/libc/kl_shim.c')).read()
    listed = set(re.findall(r'E\("([^"]+)"', shim))

    fwd = sorted(s for s in allu
                 if not s.startswith(PREFIX_SKIP)
                 and not s.startswith(PREFIX_SUBSYSTEM)
                 and not RE_SKIP.match(s)
                 and s not in SPECIAL
                 and s not in UNRESOLVED_BY_DESIGN
                 and s not in listed)

    out = os.path.join(ROOT, 'runtime/kl_libc_table.h')

    def write(names, dropped):
        with open(out, 'w') as f:
            f.write("// GENERATED by tools/gen_libc_table.py -- do not edit.\n")
            f.write("// Direct forwards: signature and struct layouts match Darwin exactly.\n")
            if dropped:
                f.write("// Imported by a guest but not USABLE on one of the two platforms this\n"
                        "// table is compiled for, so they cannot be forwarded and are left\n"
                        "// unresolved (the loader refuses them by name). The platform that\n"
                        "// refused each one is named -- visionOS both DECLARES and marks some\n"
                        "// names unavailable, which the macOS pass cannot see:\n")
                for d in sorted(dropped):
                    f.write("//   %-28s (%s)\n" % (d, REASON.get(d, "Darwin")))
            for s in names:
                f.write("KL_FWD(%s)\n" % s)

    # ...and then ASK THE COMPILER, because every rule above is about the guest
    # and none of them is about Darwin. A name qualifies for a direct forward
    # only if Darwin actually declares it, and until a second Unity title was
    # unpacked every name a guest imported happened to. SUPERHOT's libOVRLipSync
    # imports `sched_getcpu`, which is Linux-only and simply does not exist here.
    #
    # The failure this replaces is worse than a build error, which is why it is
    # worth a compile: `kl_libc_table.h` is included by kl_shim.c and was not a
    # Make prerequisite of anything, so a table with an undeclared name in it
    # did not stop the build — it stopped the REBUILD, and every gate then
    # passed against the previous binary.
    #
    # kl_shim.c is compiled rather than a synthetic probe, so the include set is
    # exactly the one the table lands in. A dropped name is NAMED, in the header
    # and on stderr: it stays unresolved, which is this project's own answer for
    # something it cannot honestly forward.
    # ...and ask it TWICE, because this table is compiled for two platforms and
    # macOS is the more permissive of them. `make xros` builds the same
    # kl_shim.c for visionOS, where a name can be DECLARED and marked
    # unavailable — clock_settime is exactly that, and libpython3.12.so in the
    # VRChat tree imports it. The macOS pass is happy; the device build then
    # fails with `'clock_settime' is unavailable: not available on visionOS`,
    # which is a build break on a platform the generator never looked at.
    #
    # Both diagnostics are collected, because "undeclared identifier" and
    # "is unavailable" are different errors and only the second one appears on
    # the visionOS pass.
    platforms = [("Darwin", ['cc', '-fsyntax-only', '-arch', 'arm64'])]
    xros_sdk = subprocess.run(['xcrun', '--sdk', 'xros', '--show-sdk-path'],
                              capture_output=True, text=True)
    if xros_sdk.returncode == 0 and xros_sdk.stdout.strip():
        platforms.append(("visionOS",
                          ['cc', '-fsyntax-only', '-target', 'arm64-apple-xros1.0',
                           '-isysroot', xros_sdk.stdout.strip(), '-arch', 'arm64']))
    else:
        # Named rather than silent: without the xrOS SDK this table can still be
        # generated, and it can still break `make xros` on a machine that has one.
        print("  NOTE: no xrOS SDK — the table is checked against macOS only, so a "
              "name that macOS declares and visionOS marks unavailable will not be "
              "caught here (it will break `make xros`)", file=sys.stderr)

    dropped = set()
    for _ in range(16):                    # bounded; each pass drops >= 1 name
        write(fwd, dropped)
        bad, err = set(), ''
        for label, cmd in platforms:
            p = subprocess.run(cmd + ['-I', os.path.join(ROOT, 'runtime'),
                                      os.path.join(ROOT, 'runtime/libc/kl_shim.c')],
                               capture_output=True, text=True)
            if p.returncode == 0:
                continue
            err = err or p.stderr
            for pat in (r"use of undeclared identifier '([A-Za-z_][A-Za-z_0-9]*)'",
                        r"'([A-Za-z_][A-Za-z_0-9]*)' is unavailable"):
                for name in re.findall(pat, p.stderr):
                    if name in fwd:
                        bad.add(name)
                        # First platform to object wins the label, and macOS is
                        # first in the list — so a name missing from BOTH reads
                        # as "Darwin" and only a visionOS-specific refusal reads
                        # as "visionOS", which is the distinction worth having.
                        REASON.setdefault(name, label)
        if not err:
            break
        if not bad:
            print("!! kl_shim.c does not compile with the generated table, and the\n"
                  "   errors are neither undeclared nor unavailable identifiers — the\n"
                  "   table is unchanged from what produced them:\n" + err[:2000],
                  file=sys.stderr)
            sys.exit(1)
        dropped |= bad
        fwd = [s for s in fwd if s not in dropped]
        for b in sorted(bad):
            print("  not usable on %s, left unresolved: %s" % (REASON[b], b),
                  file=sys.stderr)

    print("%d direct forwards -> %s" % (len(fwd), out))
    print("%d hand-written / thunked" % len(allu - set(fwd)))
    if dropped:
        print("%d imported but undeclared on Darwin (named in the header)" % len(dropped))

if __name__ == '__main__':
    main()
