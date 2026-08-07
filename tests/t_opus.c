// M1 exit criterion: libunityopus.so translated, loaded, and decoding correctly.
//
// The same roundtrip is also the P1 gate for M1b (PLANNING §12.4): hand it a
// klepton-ld-produced Mach-O dylib instead of the ELF and it must pass
// identically. Which loader runs is decided by the file's magic, not a flag, so
// the two paths are held to one test rather than two.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/klepton.h"

#define NEED(v, name) do { if (!(v)) { printf("  !! missing export: %s\n", name); return 1; } } while (0)

// 0 = ELF (mmap loader), 1 = Mach-O (translated dylib), -1 = unreadable.
static int is_macho(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    if (n != 4) return -1;
    if (!memcmp(m, "\x7f" "ELF", 4)) return 0;
    return (m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe) ? 1 : -1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "beatsaber/lib/arm64-v8a/libunityopus.so";

    kl_thread_init();                       // canary for the main thread (S0.1)

    int macho = is_macho(path);
    if (macho < 0) { printf("load failed: %s is neither ELF64 nor Mach-O\n", path); return 1; }

    printf("=== klepton M1: loading %s ===\n", path);
    printf("  loader: %s\n", macho ? "kl_load_dylib (M1b — dyld maps the guest text)"
                                   : "kl_load (M1a — mmap + runtime rewrite)");
    kl_image *img = macho ? kl_load_dylib(path) : kl_load(path);
    if (!img) { printf("load failed: %s\n", kl_error()); return 1; }

    const kl_stats *st = kl_get_stats(img);
    printf("  mapped at %p, span %zu bytes\n", kl_base(img), kl_span(img));
    printf("  relocations: RELATIVE=%u ABS64=%u GLOB_DAT=%u JUMP_SLOT=%u  (total %u)\n",
           st->relative, st->abs64, st->glob_dat, st->jump_slot,
           st->relative + st->abs64 + st->glob_dat + st->jump_slot);
    printf("  imports: %u bound, %u missing\n", st->imports_bound, st->imports_missing);
    printf("  TLS rewrites (mrs tpidr_el0 -> tpidrro_el0): %u\n", st->tls_rewrites);
    printf("  inline svc #0 sites: %u\n", st->svc_sites);
    if (st->imports_missing) { printf("  !! unresolved imports; aborting\n"); return 1; }

    printf("  running DT_INIT_ARRAY…\n");
    kl_run_init(img);
    printf("  init OK\n");

    // ---- resolve the opus API ----
    const char *(*version)(void)                            = kl_sym(img, "opus_get_version_string");
    int   (*enc_size)(int)                                  = kl_sym(img, "opus_encoder_get_size");
    void *(*enc_create)(int, int, int, int *)               = kl_sym(img, "opus_encoder_create");
    int   (*encode)(void *, const short *, int, unsigned char *, int) = kl_sym(img, "opus_encode");
    void *(*dec_create)(int, int, int *)                    = kl_sym(img, "opus_decoder_create");
    int   (*decode)(void *, const unsigned char *, int, short *, int, int) = kl_sym(img, "opus_decode");
    void  (*enc_destroy)(void *)                            = kl_sym(img, "opus_encoder_destroy");
    void  (*dec_destroy)(void *)                            = kl_sym(img, "opus_decoder_destroy");

    printf("\n=== calling into guest code ===\n");
    NEED(version, "opus_get_version_string");
    printf("  opus_get_version_string() = \"%s\"\n", version());
    NEED(enc_size, "opus_encoder_get_size");
    printf("  opus_encoder_get_size(2)   = %d\n", enc_size(2));

    // ---- real encode -> decode roundtrip ----
    NEED(enc_create, "opus_encoder_create"); NEED(encode, "opus_encode");
    NEED(dec_create, "opus_decoder_create"); NEED(decode, "opus_decode");

    const int RATE = 48000, CH = 2, FRAME = 960;     // 20 ms @ 48 kHz
    int e = 0;
    void *enc = enc_create(RATE, CH, 2049 /* OPUS_APPLICATION_AUDIO */, &e);
    if (!enc || e != 0) { printf("  !! opus_encoder_create failed (%d)\n", e); return 1; }
    void *dec = dec_create(RATE, CH, &e);
    if (!dec || e != 0) { printf("  !! opus_decoder_create failed (%d)\n", e); return 1; }
    printf("  encoder + decoder created\n");

    // a 440 Hz sine, so the roundtrip has real signal to carry
    static short pcm[960 * 2];
    for (int i = 0; i < FRAME; i++) {
        double t = (double)i / RATE;
        short v = (short)(12000.0 * __builtin_sin(2.0 * 3.14159265358979 * 440.0 * t));
        pcm[i * 2] = v; pcm[i * 2 + 1] = v;
    }

    static unsigned char packet[4000];
    int nbytes = encode(enc, pcm, FRAME, packet, sizeof packet);
    if (nbytes <= 0) { printf("  !! opus_encode failed (%d)\n", nbytes); return 1; }
    printf("  opus_encode: %d samples -> %d bytes\n", FRAME, nbytes);

    static short out[960 * 2];
    int nsamp = decode(dec, packet, nbytes, out, FRAME, 0);
    if (nsamp != FRAME) { printf("  !! opus_decode returned %d, expected %d\n", nsamp, FRAME); return 1; }

    double energy = 0;
    for (int i = 0; i < nsamp * CH; i++) energy += (double)out[i] * out[i];
    energy /= (nsamp * CH);
    printf("  opus_decode: %d samples, mean energy %.0f\n", nsamp, energy);
    if (energy < 1e5) { printf("  !! decoded signal is silent — roundtrip is wrong\n"); return 1; }

    if (enc_destroy) enc_destroy(enc);
    if (dec_destroy) dec_destroy(dec);
    kl_unload(img);

    printf("\n=== M1 EXIT CRITERION MET: guest ELF loaded, relocated and executing ===\n");
    return 0;
}
