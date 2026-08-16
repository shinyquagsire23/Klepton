// java.util.zip.ZipFile, java.io.InputStream, AVPro's crypto helpers
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include <zlib.h>
#define COMMON_DIGEST_FOR_OPENSSL 0
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonCryptor.h>
#include "kl_jni_int.h"

// ---- java.util.zip.ZipFile ----
//
// A split application binary keeps its data in an OBB, and an OBB is a zip:
// BONELAB ships main.2974 and patch.2974 beside a 75 MB APK, 6.8 GB together.
// Unity opens them through the JAVA zip classes rather than through its own
// native reader (the class name is a string in libunity, resolved lazily), so
// three methods stand between the guest and its entire asset tree.
//
// It is a reader, never a writer: nothing here creates, appends to or rewrites
// an archive. `getEntry` locates and `getInputStream` reads.
//
// The entry table is read once, from the central directory, and then the file
// is only ever seeked and read — an OBB entry can be hundreds of megabytes and
// the archive itself is larger than this process would like to hold, so a
// stream over an entry is a window (klj_stream above) rather than a buffer. The
// one exception is a DEFLATEd entry, which has to be inflated to be a window at
// all; every entry in both of BONELAB's OBBs is STORED, which is how Unity maps
// assets straight out of one, so that path is the unusual one and says so.
#define KLJ_EOCD_SIG 0x06054b50u
#define KLJ_CEN_SIG  0x02014b50u

typedef struct {
    char     name[512];
    uint16_t method;
    uint64_t csize, size, lhdr;   // ...and the LOCAL header's offset, which is
                                  // where the data is found from
} klj_zent;

typedef struct {
    char      path[1024];
    FILE     *f;
    klj_zent *e;
    unsigned  n;
} klj_zip;

static uint16_t klj_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t klj_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Returns 0 on success. The failures are all named: a zip we cannot read is
// every asset the guest has, so "it opened and found nothing" is the one answer
// that must not be possible to reach quietly.
static int klj_zip_open(klj_zip *z, const char *path) {
    snprintf(z->path, sizeof z->path, "%s", path);
    z->f = fopen(path, "rb");
    if (!z->f) { KLJ_LOG("ZipFile: cannot open %s: %s", path, strerror(errno)); return -1; }

    if (fseeko(z->f, 0, SEEK_END) != 0) return -1;
    off_t fsz = ftello(z->f);
    // The end-of-central-directory record is last, but a zip comment (up to
    // 64 KB) may follow it, so it is found by scanning back rather than read.
    size_t tail = (fsz < 66000) ? (size_t)fsz : 66000;
    uint8_t *buf = malloc(tail ? tail : 1);
    if (!buf) return -1;
    if (fseeko(z->f, fsz - (off_t)tail, SEEK_SET) != 0 || fread(buf, 1, tail, z->f) != tail) {
        free(buf); KLJ_LOG("ZipFile: short read on %s", path); return -1;
    }
    ssize_t eocd = -1;
    for (ssize_t i = (ssize_t)tail - 22; i >= 0; i--)
        if (klj_rd32(buf + i) == KLJ_EOCD_SIG) { eocd = i; break; }
    if (eocd < 0) { free(buf); KLJ_LOG("ZipFile: no end-of-central-directory in %s "
                                       "— not a zip", path); return -1; }
    uint32_t count = klj_rd16(buf + eocd + 10);
    uint32_t cdsz  = klj_rd32(buf + eocd + 12);
    uint32_t cdoff = klj_rd32(buf + eocd + 16);
    free(buf);
    // ZIP64 replaces the field it cannot hold with all-ones and puts the real
    // value in an extra record. Both OBBs here are under 4 GB and under 65535
    // entries, so this is refused BY NAME rather than half-read: a truncated
    // count reads as a working archive that is missing most of its files.
    if (count == 0xffffu || cdsz == 0xffffffffu || cdoff == 0xffffffffu) {
        KLJ_LOG("ZipFile: %s is ZIP64 (count=%u cdsz=%#x cdoff=%#x) and this "
                "reader is not — refusing rather than reading part of it",
                path, count, cdsz, cdoff);
        return -1;
    }

    uint8_t *cd = malloc(cdsz ? cdsz : 1);
    if (!cd) return -1;
    if (fseeko(z->f, (off_t)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsz, z->f) != cdsz) {
        free(cd); KLJ_LOG("ZipFile: cannot read the central directory of %s", path);
        return -1;
    }
    z->e = calloc(count, sizeof *z->e);
    if (!z->e) { free(cd); return -1; }
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cdsz; i++) {
        if (klj_rd32(cd + p) != KLJ_CEN_SIG) break;
        uint16_t nlen = klj_rd16(cd + p + 28), xlen = klj_rd16(cd + p + 30),
                 clen = klj_rd16(cd + p + 32);
        if (p + 46 + nlen > cdsz) break;
        klj_zent *e = &z->e[z->n];
        size_t take = nlen < sizeof e->name - 1 ? nlen : sizeof e->name - 1;
        memcpy(e->name, cd + p + 46, take);
        e->name[take] = '\0';
        e->method = klj_rd16(cd + p + 10);
        e->csize  = klj_rd32(cd + p + 20);
        e->size   = klj_rd32(cd + p + 24);
        e->lhdr   = klj_rd32(cd + p + 42);
        z->n++;
        p += 46u + nlen + xlen + clen;
    }
    free(cd);
    KLJ_LOG("ZipFile(\"%s\") -> %u entries", path, z->n);
    if (z->n != count)
        KLJ_LOG("ZipFile: the central directory of %s ended after %u of %u "
                "entries — the archive is truncated or malformed", path, z->n, count);
    return 0;
}

static const klj_zent *klj_zip_find(const klj_zip *z, const char *name) {
    for (unsigned i = 0; i < z->n; i++)
        if (strcmp(z->e[i].name, name) == 0) return &z->e[i];
    return NULL;
}

// Where an entry's bytes begin. The central directory records the LOCAL header's
// offset, and only that header knows how long its own name and extra fields are
// — they are allowed to differ from the central copies, so this cannot be
// computed from what we already parsed.
static int klj_zip_data_off(const klj_zip *z, const klj_zent *e, uint64_t *out) {
    uint8_t h[30];
    if (fseeko(z->f, (off_t)e->lhdr, SEEK_SET) != 0 || fread(h, 1, sizeof h, z->f) != sizeof h)
        return -1;
    if (klj_rd32(h) != 0x04034b50u) return -1;
    *out = e->lhdr + 30u + klj_rd16(h + 26) + klj_rd16(h + 28);
    return 0;
}

static klj_val klj_ZipFile_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    const char *path = n > 0 ? klj_str(a[0].l) : NULL;
    if (!path) return (klj_val){.l = NULL};
    klj_zip *z = calloc(1, sizeof *z);
    if (!z) return (klj_val){.l = NULL};
    if (klj_zip_open(z, path) != 0) {
        if (z->f) fclose(z->f);
        free(z->e); free(z);
        return (klj_val){.l = NULL};      // guest catches IOException
    }
    void *obj = kl_jni_new_object("java/util/zip/ZipFile");
    klj_as_object(obj)->data = z;
    return (klj_val){.l = obj};
}

// The entry object carries the archive too: getInputStream is given only this,
// and an entry that does not know which file it came from could be read out of
// the wrong one.
typedef struct { klj_zip *z; const klj_zent *e; } klj_zeobj;

static klj_val klj_ZipFile_getEntry(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o = klj_as_object(self);
    klj_zip    *z = o ? o->data : NULL;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!z || !name) return (klj_val){.l = NULL};
    const klj_zent *e = klj_zip_find(z, name);
    KLJ_LOG("ZipFile.getEntry(\"%s\") -> %s", name,
            e ? (e->method == 0 ? "stored" : "deflated") : "MISSING");
    if (!e) return (klj_val){.l = NULL};   // Java answers null; not an exception
    klj_zeobj *ze = calloc(1, sizeof *ze);
    ze->z = z; ze->e = e;
    return (klj_val){.l = klj_new_object_data("java/util/zip/ZipEntry", ze)};
}

static klj_val klj_ZipFile_getInputStream(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_object *eo = n > 0 ? klj_as_object(a[0].l) : NULL;
    klj_zeobj  *ze = eo ? eo->data : NULL;
    if (!ze || !ze->z || !ze->e) return (klj_val){.l = NULL};
    uint64_t off;
    if (klj_zip_data_off(ze->z, ze->e, &off) != 0) {
        KLJ_LOG("ZipFile.getInputStream(\"%s\"): no local header at %#llx",
                ze->e->name, (unsigned long long)ze->e->lhdr);
        return (klj_val){.l = NULL};
    }

    klj_stream *st = calloc(1, sizeof *st);
    if (!st) return (klj_val){.l = NULL};
    if (ze->e->method == 0) {
        // A window. Its own handle, so two streams over one archive cannot
        // disturb each other's position.
        st->f    = fopen(ze->z->path, "rb");
        st->base = off;
        st->len  = (size_t)ze->e->size;
        if (!st->f) { free(st); return (klj_val){.l = NULL}; }
    } else if (ze->e->method == 8) {
        // Inflated whole, because a window into compressed bytes is not a
        // window into anything. Named, and sized from the entry rather than
        // grown, so an implausible one is visible here rather than as memory.
        KLJ_LOG("ZipFile.getInputStream(\"%s\"): DEFLATED, inflating %llu bytes",
                ze->e->name, (unsigned long long)ze->e->size);
        uint8_t *cbuf = malloc((size_t)ze->e->csize ? (size_t)ze->e->csize : 1);
        char    *out  = malloc((size_t)ze->e->size + 1);
        if (!cbuf || !out) { free(cbuf); free(out); free(st); return (klj_val){.l = NULL}; }
        if (fseeko(ze->z->f, (off_t)off, SEEK_SET) != 0
            || fread(cbuf, 1, (size_t)ze->e->csize, ze->z->f) != (size_t)ze->e->csize) {
            free(cbuf); free(out); free(st); return (klj_val){.l = NULL};
        }
        z_stream zs; memset(&zs, 0, sizeof zs);
        zs.next_in = cbuf; zs.avail_in = (uInt)ze->e->csize;
        zs.next_out = (Bytef *)out; zs.avail_out = (uInt)ze->e->size;
        int rc = inflateInit2(&zs, -MAX_WBITS);     // raw: a zip has no zlib header
        if (rc == Z_OK) { rc = inflate(&zs, Z_FINISH); inflateEnd(&zs); }
        free(cbuf);
        if (rc != Z_STREAM_END) {
            KLJ_LOG("ZipFile.getInputStream(\"%s\"): inflate failed (%d)", ze->e->name, rc);
            free(out); free(st); return (klj_val){.l = NULL};
        }
        out[ze->e->size] = '\0';
        st->data = out;
        st->len  = (size_t)ze->e->size;
    } else {
        KLJ_LOG("ZipFile.getInputStream(\"%s\"): compression method %u is neither "
                "stored nor deflate", ze->e->name, ze->e->method);
        free(st);
        return (klj_val){.l = NULL};
    }
    KLJ_LOG("ZipFile.getInputStream(\"%s\") -> %zu bytes at %#llx",
            ze->e->name, st->len, (unsigned long long)off);
    void *obj = kl_jni_new_object("java/io/InputStream");
    klj_as_object(obj)->data = st;
    klj_own(obj, klj_stream_free);
    return (klj_val){.l = obj};
}

static klj_val klj_ZipFile_close(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_zip    *z = o ? o->data : NULL;
    // The entry table stays: a ZipEntry the guest still holds names it, and Java
    // would throw IllegalStateException on a read rather than fault. Only the
    // handle goes, which is the resource close() exists to release.
    if (z && z->f) { fclose(z->f); z->f = NULL; }
    return (klj_val){.l = NULL};
}

// The entry's own description. Three getters rather than one, because they are
// one answer asked three ways and each would otherwise be its own abort.
static klj_val klj_ZipEntry_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_zeobj  *ze = o ? o->data : NULL;
    return (klj_val){.l = ze && ze->e ? kl_jni_new_string(ze->e->name) : NULL};
}
static klj_val klj_ZipEntry_getSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_zeobj  *ze = o ? o->data : NULL;
    return (klj_val){.j = ze && ze->e ? ze->e->size : (uint64_t)-1};
}
static klj_val klj_ZipEntry_getCompressedSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_zeobj  *ze = o ? o->data : NULL;
    return (klj_val){.j = ze && ze->e ? ze->e->csize : (uint64_t)-1};
}
static klj_val klj_ZipEntry_getMethod(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_zeobj  *ze = o ? o->data : NULL;
    return (klj_val){.j = ze && ze->e ? ze->e->method : (uint64_t)-1};
}

// ---- java.io.InputStream ----
// Handed out by AssetManager.open and ZipFile.getInputStream, and until a guest
// read one through anything but Scanner it had no methods at all. The read
// family comes as a group for the usual reason: a stream that answers read([BII)
// and aborts on read([B) is not a partially-working stream, it is a guest
// wedged on the one call it happened to make.
static klj_val klj_InputStream_read_off(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o  = klj_as_object(self);
    klj_stream *st = o ? o->data : NULL;
    klj_array  *b  = n > 0 ? klj_arr(a[0].l) : NULL;
    if (!st || !b) return (klj_val){.j = (uint64_t)-1};
    int off = n > 1 ? (int)a[1].j : 0, want = n > 2 ? (int)a[2].j : b->len;
    if (off < 0 || want < 0 || off + want > b->len) return (klj_val){.j = (uint64_t)-1};
    size_t got = klj_stream_read(st, (uint8_t *)b->data + off, (size_t)want);
    // Java's end-of-stream is -1, and 0 means "asked for none" — a read that
    // answered 0 at EOF is an infinite loop in the caller.
    return (klj_val){.j = got ? (uint64_t)got : (want ? (uint64_t)-1 : 0)};
}
static klj_val klj_InputStream_read_all(void *env, void *self, const klj_val *a, int n) {
    klj_array *b = n > 0 ? klj_arr(a[0].l) : NULL;
    klj_val args[3] = { a[0], {.j = 0}, {.j = b ? (uint64_t)b->len : 0} };
    return klj_InputStream_read_off(env, self, args, 3);
}
static klj_val klj_InputStream_read1(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_stream *st = o ? o->data : NULL;
    uint8_t c;
    return (klj_val){.j = klj_stream_read(st, &c, 1) == 1 ? (uint64_t)c : (uint64_t)-1};
}
static klj_val klj_InputStream_skip(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o  = klj_as_object(self);
    klj_stream *st = o ? o->data : NULL;
    int64_t want = n > 0 ? (int64_t)a[0].j : 0;
    if (!st || want <= 0) return (klj_val){.j = 0};
    size_t left = st->len - st->pos;
    size_t take = (size_t)want < left ? (size_t)want : left;
    st->pos += take;
    return (klj_val){.j = (uint64_t)take};
}
static klj_val klj_InputStream_available(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_stream *st = o ? o->data : NULL;
    return (klj_val){.j = st ? (uint64_t)(st->len - st->pos) : 0};
}
static klj_val klj_InputStream_close(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_stream *st = o ? o->data : NULL;
    klj_stream_close(st);
    return (klj_val){.l = NULL};
}

// ---- AVPro Video's Java crypto helpers ----
//
// VRChat ships RenderHeads' AVPro Video, whose native half keeps five crypto
// primitives on the JAVA side and calls down to them: a digest, a KDF, and a
// three-call streaming cipher. They are reached from libAVProVideo2Native's
// JNI_OnLoad — the plugin PROBES them at load, before any video exists — so
// they are not on some encrypted-playback branch a host run would never take.
// They are the first thing between this guest and a frame.
//
// Every one is transcribed from com/renderheads/AVPro/Video/Manager.smali, and
// that matters more here than usual: a cipher is entirely specification. Get
// the mode, the padding, the key length or the bit-vs-byte convention wrong and
// each call still SUCCEEDS and returns plausible bytes, which is a failure with
// no error surface anywhere near it.
//
// What this is NOT is an ownership check. The guest supplies its own key and
// its own IV and decrypts its own media with them, so this is a primitive in
// the same category as zlib or opus — the DRM line (see kl_ovrplat.c) is about
// entitlement and delivery, and nothing here asks or answers either.
//
// AVPro Video's Manager.SHA256(byte[]) -> byte[]. Its Java body is three lines
// — MessageDigest.getInstance("SHA-256"), update, digest — so this is a pure
// function of its argument and nothing about it is invented; CommonCrypto
// computes the same 32 bytes java.security would.
//
// Answering it rather than stubbing it matters: the caller is the plugin's own
// integrity path, and a digest we make up is a digest that will not match
// whatever it is compared against, which is a failure several layers away from
// here. A NULL argument gets the digest of the empty input, which is what
// MessageDigest.update(null) would NOT do — but the guest never passes one, and
// answering the empty digest is closer than answering null.
static klj_val klj_AVPro_SHA256(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_array *in = n > 0 ? klj_arr(a[0].l) : NULL;
    void *out = klj_new_array('B', NULL, CC_SHA256_DIGEST_LENGTH);
    CC_SHA256(in ? in->data : "", in ? (CC_LONG)in->len : 0, klj_arr(out)->data);
    return (klj_val){.l = out};
}

// ...and its sibling, Manager.KeyDerivationPBKDF(password, salt, iterations,
// out) -> the number of bytes written. Transcribed from the same class's smali
// rather than guessed, because every parameter of a KDF is load-bearing:
//
//   algorithm   PBKDF2WithHmacSHA256
//   password    the bytes of `password`, all `capacity()` of them. Java decodes
//               them to a char[] and PBEKeySpec re-encodes as UTF-8, so the
//               round trip is the identity — and it is only the identity
//               because the guest sizes the char[] at the buffer's CAPACITY,
//               which throws unless one char came out per byte.
//   salt        the bytes of `salt`, all of its capacity
//   rounds      the int, verbatim
//   dkLen       out.capacity() — Java asks for `* 8` because PBEKeySpec counts
//               BITS and CommonCrypto counts bytes. Getting that conversion
//               backwards produces a key of the right length and wrong content.
//
// All three buffers are direct, so they are guest memory and the derived key is
// written where the guest is already looking.
static klj_val klj_AVPro_KeyDerivationPBKDF(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n < 4) return (klj_val){.j = (uint64_t)-1};
    klj_direct_buffer *pw = klj_direct(a[0].l), *salt = klj_direct(a[1].l),
                      *out = klj_direct(a[3].l);
    unsigned rounds = (unsigned)a[2].j;
    if (!pw || !salt || !out) return (klj_val){.j = (uint64_t)-1};
    int rc = CCKeyDerivationPBKDF(kCCPBKDF2, pw->addr, (size_t)pw->capacity,
                                  salt->addr, (size_t)salt->capacity,
                                  kCCPRFHmacAlgSHA256, rounds,
                                  out->addr, (size_t)out->capacity);
    // Java returns -1 on failure and the key length on success; a partially
    // written output buffer would be a key the guest cannot tell from a good one.
    return (klj_val){.j = rc == kCCSuccess ? (uint64_t)out->capacity : (uint64_t)-1};
}

// AES-CBC with no padding, the algorithm Manager.CreateCryptor names, and
// DECRYPT only: its own body refuses every other mode with "Only decrypt mode
// is supported currently", so an encryptor here would be a capability the guest
// declines to have.
typedef struct {
    CCCryptorRef ref;
    uint8_t      iv[kCCBlockSizeAES128];
    int          has_iv;
} klj_cryptor;

static const char KLJ_CLASS_CIPHER[] = "javax/crypto/Cipher";

static klj_cryptor *klj_cryptor_of(void *o_) {
    klj_object *o = klj_as_object(o_);
    return (o && strcmp(o->cls, KLJ_CLASS_CIPHER) == 0) ? o->data : NULL;
}

// CreateCryptor(opmode, key, iv) -> Cipher, or null. opmode is
// javax.crypto.Cipher.DECRYPT_MODE (2); the guest's own code returns null for
// anything else, so we do too rather than widening its contract.
static klj_val klj_AVPro_CreateCryptor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n < 3 || (int)a[0].j != 2) return (klj_val){.l = NULL};
    klj_array *key = klj_arr(a[1].l), *iv = klj_arr(a[2].l);
    if (!key || !iv || iv->len != kCCBlockSizeAES128) return (klj_val){.l = NULL};

    klj_cryptor *c = calloc(1, sizeof *c);
    if (!c) return (klj_val){.l = NULL};
    memcpy(c->iv, iv->data, sizeof c->iv);
    c->has_iv = 1;
    // ccNoPadding: "AES/CBC/NoPadding" says so, and padding is the one option
    // whose effect is invisible until the LAST block of a stream.
    if (CCCryptorCreate(kCCDecrypt, kCCAlgorithmAES, 0 /* no padding */,
                        key->data, (size_t)key->len, c->iv, &c->ref) != kCCSuccess) {
        free(c);
        return (klj_val){.l = NULL};   // Java logs and returns null here too
    }
    return (klj_val){.l = klj_new_object_data(KLJ_CLASS_CIPHER, c)};
}

// UpdateCryptor(cipher, in, out) -> bytes written. Both buffers are direct, so
// they are guest memory; this project models a direct ByteBuffer as address +
// capacity, and Java's Cipher.update(ByteBuffer, ByteBuffer) consumes
// in.remaining() — for a buffer that has never had its position moved, that is
// its capacity.
//
// A too-small output is the guest's own "outBuf is not large enough" branch,
// which logs and returns 0 rather than throwing; CCCryptorGetOutputLength asks
// the same question CommonCrypto would answer with kCCBufferTooSmall.
static klj_val klj_AVPro_UpdateCryptor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n < 3) return (klj_val){.j = 0};
    klj_cryptor *c = klj_cryptor_of(a[0].l);
    klj_direct_buffer *in = klj_direct(a[1].l), *out = klj_direct(a[2].l);
    if (!c || !in || !out) return (klj_val){.j = 0};
    size_t moved = 0;
    if (CCCryptorUpdate(c->ref, in->addr, (size_t)in->capacity,
                        out->addr, (size_t)out->capacity, &moved) != kCCSuccess)
        return (klj_val){.j = 0};
    return (klj_val){.j = (uint64_t)moved};
}

// FinaliseCryptor(cipher, out) -> bytes written. Java calls doFinal(), which
// with NoPadding and a block-aligned stream yields nothing — and which RESETS
// the cipher to its post-init state, ready for the next stream under the same
// key. CCCryptorReset(ref, iv) is that reset exactly, which is why the IV is
// kept on the record: without it the next Update would silently chain from the
// wrong block and produce garbage that decrypts without error.
static klj_val klj_AVPro_FinaliseCryptor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n < 2) return (klj_val){.j = 0};
    klj_cryptor *c = klj_cryptor_of(a[0].l);
    klj_direct_buffer *out = klj_direct(a[1].l);
    if (!c || !out) return (klj_val){.j = 0};
    size_t moved = 0;
    if (CCCryptorFinal(c->ref, out->addr, (size_t)out->capacity, &moved) != kCCSuccess)
        moved = 0;
    CCCryptorReset(c->ref, c->has_iv ? c->iv : NULL);
    return (klj_val){.j = (uint64_t)moved};
}

const klj_binding klj_bind_io[] = {
    {"com/renderheads/AVPro/Video/Manager", "SHA256", "([B)[B", klj_AVPro_SHA256},
    {"com/renderheads/AVPro/Video/Manager", "KeyDerivationPBKDF",
     "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;ILjava/nio/ByteBuffer;)I", klj_AVPro_KeyDerivationPBKDF},
    {"com/renderheads/AVPro/Video/Manager", "CreateCryptor", "(I[B[B)Ljavax/crypto/Cipher;",
     klj_AVPro_CreateCryptor},
    {"com/renderheads/AVPro/Video/Manager", "UpdateCryptor",
     "(Ljavax/crypto/Cipher;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)I", klj_AVPro_UpdateCryptor},
    {"com/renderheads/AVPro/Video/Manager", "FinaliseCryptor",
     "(Ljavax/crypto/Cipher;Ljava/nio/ByteBuffer;)I", klj_AVPro_FinaliseCryptor},
    // The OBB, for a split application binary (BONELAB). A reader only.
    {"java/util/zip/ZipFile", "<init>",   "(Ljava/lang/String;)V", klj_ZipFile_init},
    {"java/util/zip/ZipFile", "getEntry", "(Ljava/lang/String;)Ljava/util/zip/ZipEntry;",
     klj_ZipFile_getEntry},
    {"java/util/zip/ZipFile", "getInputStream", "(Ljava/util/zip/ZipEntry;)Ljava/io/InputStream;",
     klj_ZipFile_getInputStream},
    {"java/util/zip/ZipFile", "close",    "()V",                   klj_ZipFile_close},
    {"java/util/zip/ZipEntry", "getName", "()Ljava/lang/String;",  klj_ZipEntry_getName},
    {"java/util/zip/ZipEntry", "getSize", "()J",                   klj_ZipEntry_getSize},
    {"java/util/zip/ZipEntry", "getCompressedSize", "()J",         klj_ZipEntry_getCompressedSize},
    {"java/util/zip/ZipEntry", "getMethod", "()I",                 klj_ZipEntry_getMethod},
    {"java/io/InputStream", "read",      "([BII)I", klj_InputStream_read_off},
    {"java/io/InputStream", "read",      "([B)I",   klj_InputStream_read_all},
    {"java/io/InputStream", "read",      "()I",     klj_InputStream_read1},
    {"java/io/InputStream", "skip",      "(J)J",    klj_InputStream_skip},
    {"java/io/InputStream", "available", "()I",     klj_InputStream_available},
    {"java/io/InputStream", "close",     "()V",     klj_InputStream_close},
    {0}
};
