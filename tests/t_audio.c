// The guest's sample format, both halves of it.
//
// This exists because of Batman: Arkham Shadow, whose Wwise sink renders float32
// and Oboe therefore opens OpenSL with SLAndroidDataFormat_PCM_EX; kl_opensl.c
// recognised only SLDataFormat_PCM, fell through to its 48000/2/16 defaults,
// and handed float bytes to an int16 resampler. Every 4-byte float became two
// int16 samples, so the stream played at half speed with the mantissa half in
// one ear (full-scale white noise) and the exponent half in the other. The
// crucial part: the [sl] log line read "48000 Hz, 2 ch, 16 bit", which is
// exactly what a correct FMOD player reads, so nothing in the log disagreed
// with anything else.
//
// Two gates, in the two places that failure lived:
//
//   1. kl_audio_to_float — the widening itself, per format, against known
//      values and a known sine.
//   2. the OpenSL surface — drive slCreateEngine/CreateAudioPlayer exactly as
//      the guest does, with a PCM_EX float source, and require that the format
//      it reports back is float and not the default.
//
// KL_AUDIO=0 throughout: this is a host test and must not try to open a real
// output unit, and the format parsing happens before the device either way.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "media/kl_audio.h"
#include "media/kl_opensl.h"

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ---------------------------------------------------------------------------
// 1. the widening

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_to_float(void) {
    printf("=== kl_audio_to_float ===\n");

    CHECK(kl_audio_fmt_bytes(KL_AUDIO_S16) == 2, "s16 is not 2 bytes");
    CHECK(kl_audio_fmt_bytes(KL_AUDIO_F32) == 4, "f32 is not 4 bytes");
    CHECK(kl_audio_fmt_bytes(KL_AUDIO_S24) == 3, "s24 is not 3 bytes");
    CHECK(kl_audio_fmt_bytes(KL_AUDIO_U8)  == 1, "u8 is not 1 byte");
    CHECK(strcmp(kl_audio_fmt_name(KL_AUDIO_F32), "f32") == 0, "f32 has the wrong name");

    float out[8];

    // int16: full negative is exactly -1, full positive one step short of it.
    const int16_t s16[] = {0, 32767, -32768, 16384};
    kl_audio_to_float(KL_AUDIO_S16, s16, out, 4);
    CHECK(near(out[0], 0.0f),        "s16 0 -> %f", out[0]);
    CHECK(near(out[1], 32767.0f / 32768.0f), "s16 +full -> %f", out[1]);
    CHECK(near(out[2], -1.0f),       "s16 -full -> %f", out[2]);
    CHECK(near(out[3], 0.5f),        "s16 half -> %f", out[3]);

    // float32 passes through, and is clamped rather than wrapped.
    const float f32[] = {0.0f, 0.5f, -1.0f, 4.0f, -4.0f};
    kl_audio_to_float(KL_AUDIO_F32, f32, out, 5);
    CHECK(near(out[0], 0.0f),  "f32 0 -> %f", out[0]);
    CHECK(near(out[1], 0.5f),  "f32 0.5 -> %f", out[1]);
    CHECK(near(out[2], -1.0f), "f32 -1 -> %f", out[2]);
    CHECK(near(out[3], 1.0f),  "f32 +4 should clamp to +1, got %f", out[3]);
    CHECK(near(out[4], -1.0f), "f32 -4 should clamp to -1, got %f", out[4]);

    // ...and a NaN becomes silence, not a full-scale spike.
    const float nanbuf[] = {NAN};
    kl_audio_to_float(KL_AUDIO_F32, nanbuf, out, 1);
    CHECK(out[0] == 0.0f, "f32 NaN -> %f (should be silence)", out[0]);

    // int32 and packed int24 share a scale.
    const int32_t s32[] = {0, 1073741824, -2147483647 - 1};
    kl_audio_to_float(KL_AUDIO_S32, s32, out, 3);
    CHECK(near(out[0], 0.0f),  "s32 0 -> %f", out[0]);
    CHECK(near(out[1], 0.5f),  "s32 half -> %f", out[1]);
    CHECK(near(out[2], -1.0f), "s32 -full -> %f", out[2]);

    // 24-bit little-endian: 0x000000, 0x400000 (+0.5), 0x800000 (-1.0).
    const uint8_t s24[] = {0x00, 0x00, 0x00,  0x00, 0x00, 0x40,  0x00, 0x00, 0x80};
    kl_audio_to_float(KL_AUDIO_S24, s24, out, 3);
    CHECK(near(out[0], 0.0f),  "s24 0 -> %f", out[0]);
    CHECK(near(out[1], 0.5f),  "s24 half -> %f", out[1]);
    CHECK(near(out[2], -1.0f), "s24 -full -> %f", out[2]);

    // unsigned 8-bit is offset-binary: 0x80 is silence.
    const uint8_t u8[] = {128, 255, 0, 192};
    kl_audio_to_float(KL_AUDIO_U8, u8, out, 4);
    CHECK(near(out[0], 0.0f),  "u8 silence -> %f", out[0]);
    CHECK(near(out[2], -1.0f), "u8 -full -> %f", out[2]);
    CHECK(near(out[3], 0.5f),  "u8 half -> %f", out[3]);

    // The end-to-end shape the bug actually broke: a full-scale sine, widened
    // from float and from int16, must land on the same waveform. The old code
    // read the float buffer as int16 and got white noise at ~full scale in one
    // interleaved lane, which this comparison catches by RMS alone.
    enum { N = 480 };
    float  sine_f[N];
    int16_t sine_i[N];
    for (int i = 0; i < N; i++) {
        double v = 0.75 * sin(2.0 * M_PI * 440.0 * i / 48000.0);
        sine_f[i] = (float)v;
        sine_i[i] = (int16_t)lrint(v * 32767.0);
    }
    float from_f[N], from_i[N];
    kl_audio_to_float(KL_AUDIO_F32, sine_f, from_f, N);
    kl_audio_to_float(KL_AUDIO_S16, sine_i, from_i, N);

    double err = 0.0, peak_f = 0.0;
    for (int i = 0; i < N; i++) {
        double d = from_f[i] - from_i[i];
        err += d * d;
        if (fabs(from_f[i]) > peak_f) peak_f = fabs(from_f[i]);
    }
    err = sqrt(err / N);
    printf("  440 Hz sine: peak %.3f, float-vs-int16 RMS error %.6f\n", peak_f, err);
    CHECK(fabs(peak_f - 0.75) < 0.01, "float sine peak is %.3f, expected 0.75", peak_f);
    CHECK(err < 1e-3, "float and int16 widening disagree by %.6f RMS", err);
}

// ---------------------------------------------------------------------------
// 2. the OpenSL surface
//
// The ABI, transcribed the same way kl_opensl.c transcribes it. A test that
// shared the runtime's private declarations would not be testing the surface
// the guest sees, which is the only surface that has ever been wrong here.

typedef uint32_t SLuint32;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef struct SLInterfaceID_ *SLInterfaceID;

struct SLObjectItf_;
typedef const struct SLObjectItf_ *const *SLObjectItf;
struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf, SLboolean);
    SLresult (*Resume)(SLObjectItf, SLboolean);
    SLresult (*GetState)(SLObjectItf, SLuint32 *);
    SLresult (*GetInterface)(SLObjectItf, const SLInterfaceID, void *);
    SLresult (*RegisterCallback)(SLObjectItf, void *, void *);
    void     (*AbortAsyncOperation)(SLObjectItf);
    void     (*Destroy)(SLObjectItf);
    SLresult (*SetPriority)(SLObjectItf, int32_t, SLboolean);
    SLresult (*GetPriority)(SLObjectItf, int32_t *);
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf, int16_t, SLInterfaceID *, SLboolean);
};

struct SLEngineItf_;
typedef const struct SLEngineItf_ *const *SLEngineItf;
struct SLEngineItf_ {
    void *CreateLEDDevice, *CreateVibraDevice;
    SLresult (*CreateAudioPlayer)(SLEngineItf, SLObjectItf *, void *, void *,
                                  SLuint32, const SLInterfaceID *, const SLboolean *);
};

typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endianness;
} SLDataFormat_PCM;
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endianness, representation;
} SLAndroidDataFormat_PCM_EX;

#define SL_DATAFORMAT_PCM 2
#define SL_ANDROID_DATAFORMAT_PCM_EX 0x4
#define SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT 0x1
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT      0x3

typedef SLresult (*sl_create_engine_fn)(SLObjectItf *, SLuint32, void *, SLuint32,
                                        const SLInterfaceID *, const SLboolean *);

// The runtime announces the parsed format on stderr and nowhere else, which is
// the point — the on-device log line is the artifact someone will read later.
// So the test reads exactly that line, and would have failed on the shipped
// build for saying "16 bit" where the guest asked for float.
static char g_line[512];

static int create_player(void *fmt, const char *what) {
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    FILE *tmp = tmpfile();
    if (!tmp || saved < 0) { printf("  FAIL: cannot capture stderr\n"); g_fail++; return -1; }
    dup2(fileno(tmp), STDERR_FILENO);

    sl_create_engine_fn create = (sl_create_engine_fn)kl_opensl_sym("slCreateEngine");
    SLObjectItf eng = NULL;
    SLresult r = create ? create(&eng, 0, NULL, 0, NULL, NULL) : 1;
    SLEngineItf ei = NULL;
    // dlsym hands back the ADDRESS of the SL_IID_* variable, not its value —
    // one level of indirection the guest also has to get right.
    SLInterfaceID *iid_engine = kl_opensl_sym("SL_IID_ENGINE");
    if (r == 0 && eng && iid_engine)
        r = (*eng)->GetInterface(eng, *iid_engine, &ei);
    SLObjectItf player = NULL;
    SLDataSource src = { NULL, fmt };
    if (r == 0 && ei)
        r = (*ei)->CreateAudioPlayer(ei, &player, &src, NULL, 0, NULL, NULL);

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    rewind(tmp);
    g_line[0] = 0;
    char buf[512];
    while (fgets(buf, sizeof buf, tmp))
        if (strstr(buf, "[sl] audio player:")) {
            snprintf(g_line, sizeof g_line, "%s", buf);
            char *nl = strchr(g_line, '\n'); if (nl) *nl = 0;
        }
    fclose(tmp);

    if (r != 0) { printf("  FAIL: %s: CreateAudioPlayer returned %u\n", what, r); g_fail++; return -1; }
    if (!g_line[0]) { printf("  FAIL: %s: no [sl] audio player line\n", what); g_fail++; return -1; }
    printf("  %s ->%s\n", what, strstr(g_line, "player:") + 7);
    return 0;
}

static void test_opensl_format(void) {
    printf("=== OpenSL data formats ===\n");

    // Plain PCM, 16-bit — FMOD's, and the path that must not change.
    SLDataFormat_PCM pcm = { SL_DATAFORMAT_PCM, 2, 44100 * 1000, 16, 16, 3, 2 };
    if (create_player(&pcm, "PCM 44100/2/16") == 0) {
        CHECK(strstr(g_line, "44100 Hz") != NULL, "PCM rate not read as 44100 (milliHz slipped?)");
        CHECK(strstr(g_line, "2 ch") != NULL, "PCM channels not read as 2");
        CHECK(strstr(g_line, "s16") != NULL, "PCM 16-bit did not come out as s16");
    }

    // PCM_EX float — Oboe's, when the guest asks for Float. THE regression.
    SLAndroidDataFormat_PCM_EX ex = { SL_ANDROID_DATAFORMAT_PCM_EX, 2, 48000 * 1000, 32,
                                      32, 3, 2, SL_ANDROID_PCM_REPRESENTATION_FLOAT };
    if (create_player(&ex, "PCM_EX 48000/2/32 float") == 0) {
        CHECK(strstr(g_line, "48000 Hz") != NULL, "PCM_EX rate not read as 48000");
        CHECK(strstr(g_line, "f32") != NULL,
              "PCM_EX float was NOT recognised — float played as int16: %s", g_line);
        CHECK(strstr(g_line, "16 bit") == NULL,
              "PCM_EX float still reports 16 bit: %s", g_line);
    }

    // PCM_EX signed int, 16-bit — same struct, and must stay int16.
    SLAndroidDataFormat_PCM_EX exi = { SL_ANDROID_DATAFORMAT_PCM_EX, 1, 24000 * 1000, 16,
                                       16, 1, 2, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT };
    if (create_player(&exi, "PCM_EX 24000/1/16 signed") == 0) {
        CHECK(strstr(g_line, "24000 Hz") != NULL, "PCM_EX int rate not read as 24000");
        CHECK(strstr(g_line, "1 ch") != NULL, "PCM_EX int channels not read as 1");
        CHECK(strstr(g_line, "s16") != NULL, "PCM_EX signed 16-bit did not come out as s16");
    }

    // PCM_EX signed int, 32-bit — the third representation, and NOT float.
    SLAndroidDataFormat_PCM_EX ex32 = { SL_ANDROID_DATAFORMAT_PCM_EX, 2, 48000 * 1000, 32,
                                        32, 3, 2, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT };
    if (create_player(&ex32, "PCM_EX 48000/2/32 signed") == 0)
        CHECK(strstr(g_line, "s32") != NULL, "PCM_EX signed 32-bit did not come out as s32");
}

int main(void) {
    setenv("KL_AUDIO", "0", 1);   // host test: never open a real output unit
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== audio sample formats ===\n");
    test_to_float();
    test_opensl_format();
    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("audio formats OK\n");
    return 0;
}
