// Sound out of a Linux guest: SDL's "disk" driver, with a host on the other end.
//
// A guest that plays through SDL can be told to use any of SDL's own backends,
// and every real one (PulseAudio, PipeWire, ALSA) is a daemon or a kernel
// interface with a large surface that does not exist here. The disk driver has
// none: it writes the device's raw PCM to a file. So the guest is pointed at
// /dev/klepton-audio (SDL_AUDIO_DRIVER=disk, SDL_AUDIO_DISK_OUTPUT_FILE), the
// open of that path is answered with one end of a socket pair, and a host
// thread plays what arrives through kl_audio — the same CoreAudio sink every
// other guest's sound ends in.
//
// Two details make it a clock rather than a recording:
//
//   * SDL_AUDIO_DISK_TIMESCALE=0. The driver otherwise sleeps a buffer's length
//     between writes, a clock of its own that runs ~10% slow (measured: 316 kB/s
//     of a 353 kB/s stream) and would underrun the device for ever. With no
//     sleep it writes as fast as the socket takes it, and
//   * the socket's buffer is small, so it takes it exactly as fast as the
//     device plays it: kl_audio_write blocks for the duration of what it is
//     given, the reader stops reading, the writer blocks. The device is the
//     only clock and the latency is one small buffer.
//
// The format is whatever the guest opened its device with, and the driver does
// not say so in the stream. Half-Life: Alyx opens 32-bit float stereo at
// 44.1 kHz (SDL's own log line, from a host run); KL_LX_AUDIO_FORMAT=
// "<f32|s16>,<channels>,<rate>" names another.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../media/kl_audio.h"
#include "../kl_env.h"

#define LXA_PATH "/dev/klepton-audio"

static int      g_float = 1;
static unsigned g_channels = 2, g_rate = 44100;

int kl_lx_audio_is(const char *path) { return path && !strcmp(path, LXA_PATH); }

static void *reader(void *arg) {
    int fd = (int)(intptr_t)arg;
    pthread_setname_np("Klepton Linux audio");
    if (kl_audio_open(g_rate, g_channels, 16) != 0) {
        fprintf(stderr, "  [klepton] linux audio: the output device would not open — "
                        "the guest's sound is read and dropped\n");
    } else {
        kl_audio_play();
        fprintf(stderr, "  [klepton] linux audio: playing the guest's %s %u-channel %u Hz "
                        "stream\n", g_float ? "float" : "16-bit", g_channels, g_rate);
    }
    const size_t in_frame = (g_float ? 4 : 2) * g_channels;
    uint8_t in[8192];
    int16_t out[sizeof in / 2];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, in + have, sizeof in - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                          // the guest closed its device
        have += (size_t)n;
        size_t frames = have / in_frame, samples = frames * g_channels;
        if (!frames) continue;
        const void *pcm = in;
        if (g_float) {
            const float *f = (const float *)in;
            for (size_t i = 0; i < samples; i++) {
                float v = f[i];
                if (!(v == v)) v = 0;               // NaN
                if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                out[i] = (int16_t)(v * 32767.0f);
            }
            pcm = out;
        }
        if (kl_audio_active()) kl_audio_write(pcm, samples * 2);
        size_t used = frames * in_frame;
        memmove(in, in + used, have - used);
        have -= used;
    }
    close(fd);
    kl_audio_pause();
    fprintf(stderr, "  [klepton] linux audio: the guest closed its device\n");
    return NULL;
}

// The guest's end of a new pair, with a reader started on the other.
int kl_lx_audio_open(void) {
    const char *fmt = kl_env_str("KL_LX_AUDIO_FORMAT", NULL);
    if (fmt) {
        char kind[8] = "";
        unsigned ch = 0, rate = 0;
        if (sscanf(fmt, "%7[^,],%u,%u", kind, &ch, &rate) == 3 && ch >= 1 && ch <= 8 &&
            rate >= 8000 && rate <= 192000) {
            g_float = !strcmp(kind, "f32");
            g_channels = ch;
            g_rate = rate;
        } else {
            fprintf(stderr, "  [klepton] linux audio: KL_LX_AUDIO_FORMAT=%s is not "
                            "<f32|s16>,<channels>,<rate> — ignored\n", fmt);
        }
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    // About 20 ms of the stream: enough that the writer is not woken per
    // sample, small enough to be the pacing rather than a queue.
    int buf = (int)((g_float ? 4 : 2) * g_channels * g_rate / 50);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    int rc = pthread_create(&th, &attr, reader, (void *)(intptr_t)sv[1]);
    pthread_attr_destroy(&attr);
    if (rc != 0) { close(sv[0]); close(sv[1]); errno = rc; return -1; }
    pthread_detach(th);
    return sv[0];
}
