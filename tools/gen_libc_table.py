#!/usr/bin/env python3
"""Regenerate runtime/kl_libc_table.h — the direct bionic->Darwin forwards.

A symbol qualifies for a direct forward only if its signature AND every struct it
touches are identical on both platforms. Everything else is hand-written in
kl_libc.c / kl_pthread.c / kl_dl.c, or thunked in kl_va_thunks.S.
"""
import struct, re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Both targets, and for Steam Link BOTH front doors — the streaming client and
# the 2D configuration frontend. See the fourth entry for why the frontend is no
# longer excluded.
TARGETS = [
    (os.path.join(ROOT, 'beatsaber/lib/arm64-v8a'),
     ['libmain', 'lib_burst_generated', 'libunityopus', 'libunity', 'libil2cpp']),
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

def undefined_symbols(path):
    d = open(path, 'rb').read()
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
dup2
""".split())

# Subsystem gateways with their own files, like gl*/SL_* below: AAudio is the
# VR build's audio API (kl_audio.c's sink already exists; the API in front of it
# does not), and xr* is the OpenXR runtime we must REPLACE rather than forward —
# libopenxr_loader.so talks to an Android runtime broker that does not exist
# here, exactly as libOVRPlugin.so did (PLANNING §3.1).
PREFIX_SUBSYSTEM = ('AAudio', 'xr')

PREFIX_SKIP = ('pthread_', 'sem_', '__android_log', 'egl')
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

def main():
    allu, exported = set(), set()
    for libs, names in TARGETS:
        for f in names:
            allu |= undefined_symbols(os.path.join(libs, f + '.so'))
            exported |= defined_symbols(os.path.join(libs, f + '.so'))
    # A name one guest library exports is not a shim gap — libmain.so's 1418
    # unresolved import sites are overwhelmingly SDL3's, and they bind against
    # the guest libSDL3 at relocation time (kl_guest_sym_global). Generating a
    # forward for one would silently prefer the host's symbol of the same name.
    allu -= exported
    shim = open(os.path.join(ROOT, 'runtime/kl_shim.c')).read()
    listed = set(re.findall(r'E\("([^"]+)"', shim))

    fwd = sorted(s for s in allu
                 if not s.startswith(PREFIX_SKIP)
                 and not s.startswith(PREFIX_SUBSYSTEM)
                 and not RE_SKIP.match(s)
                 and s not in SPECIAL
                 and s not in listed)

    out = os.path.join(ROOT, 'runtime/kl_libc_table.h')
    with open(out, 'w') as f:
        f.write("// GENERATED by tools/gen_libc_table.py -- do not edit.\n")
        f.write("// Direct forwards: signature and struct layouts match Darwin exactly.\n")
        for s in fwd:
            f.write("KL_FWD(%s)\n" % s)
    print("%d direct forwards -> %s" % (len(fwd), out))
    print("%d hand-written / thunked" % len(allu - set(fwd)))

if __name__ == '__main__':
    main()
