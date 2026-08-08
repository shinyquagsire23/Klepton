#!/usr/bin/env python3
"""Regenerate runtime/kl_libc_table.h — the direct bionic->Darwin forwards.

A symbol qualifies for a direct forward only if its signature AND every struct it
touches are identical on both platforms. Everything else is hand-written in
kl_libc.c / kl_pthread.c / kl_dl.c, or thunked in kl_va_thunks.S.
"""
import struct, re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Both targets. Steam Link's working set only — libshell and Qt are out of
# scope (PLANNING §11.2), and pulling them in would generate forwards for a
# 22 MB dependency nothing loads.
TARGETS = [
    (os.path.join(ROOT, 'beatsaber/lib/arm64-v8a'),
     ['libmain', 'lib_burst_generated', 'libunityopus', 'libunity', 'libil2cpp']),
    (os.path.join(ROOT, 'steamlink-android/lib/arm64-v8a'),
     ['libmain', 'libSDL3', 'libSDL3_ttf', 'libSDL3_image',
      'libh264bitstream', 'libhevcbitstream', 'libc++_shared']),
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
swprintf vprintf vsscanf execl syscall sysconf fopen access mkdir unlink rename
dlopen dlsym dlclose dlerror dladdr dl_iterate_phdr
memrchr memalign
fputc fputs fwrite fread fclose fflush fgets getc feof ferror clearerr
fseek fseeko ftell ftello setvbuf
__memcpy_chk __memset_chk __strcpy_chk __strlen_chk __strchr_chk __vsnprintf_chk
__stack_chk_fail
clock_gettime clock_getres gettimeofday

getauxval fegetenv fesetenv feholdexcept feupdateenv statvfs fstatvfs sendfile
epoll_create epoll_create1 epoll_ctl epoll_wait openat __open_2
__memmove_chk __strncpy_chk __strncpy_chk2 __strcat_chk __read_chk __vsprintf_chk
sincosf sincos putchar getchar fdatasync __cmsg_nxthdr __cxa_thread_atexit_impl
fileno fgetc ungetc getwc fgetwc ungetwc fputwc putwc fwide
stat64 lstat64 fstat64
""".split())

PREFIX_SKIP = ('pthread_', 'sem_', '__android_log', 'egl')
# Same reason gl* is excluded: these are subsystem gateways with their own
# files, not libc. SL_/sl (OpenSL ES) -> kl_opensl.c, AMedia*/AMEDIA* ->
# kl_mediandk.c, SDL_/IMG_/TTF_ are the guest's own cross-library imports and
# bind against the guest libSDL3.
RE_SKIP = re.compile(r'^(AAsset|ALooper|ANative|ASensor|AConfig|AInput|AMedia|AMEDIA'
                     r'|SL_|sl[A-Z]|SDL_|IMG_|TTF_|Mix_|gl[A-Z])')

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
